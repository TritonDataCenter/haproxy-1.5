/*
 * FD polling functions for SunOS event ports.
 *
 * Copyright 2017 Joyent, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

/*
 * The assertions in this file are cheap and we always want them enabled.
 */
#ifdef NDEBUG
#undef NDEBUG
#include <assert.h>
#define NDEBUG
#else
#include <assert.h>
#endif

#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>

#include <poll.h>
#include <port.h>
#include <errno.h>
#include <syslog.h>

#include <common/compat.h>
#include <common/config.h>
#include <common/ticks.h>
#include <common/time.h>

#include <types/global.h>

#include <proto/fd.h>
#include <proto/signal.h>
#include <proto/task.h>
#include <proto/log.h>

/*
 * Private data:
 */
static int evports_fd = -1;
static port_event_t *evports_evlist = NULL;
static int evports_evlist_max = 0;
static int volatile evports_panic_errno = 0;

/*
 * Convert the "state" member of "fdtab" into an event ports event mask.
 */
static int evports_state_to_events(int state)
{
	int events = 0;

	if (state & FD_EV_POLLED_W)
		events |= POLLOUT;
	if (state & FD_EV_POLLED_R)
		events |= POLLIN;

	return (events);
}

/*
 * Associate or dissociate this file descriptor with the event port, using the
 * specified event mask.  We are strict with failures, to ensure that we're not
 * doing extra work.
 */
static void evports_resync_fd(int fd, int events)
{
	if (events == 0) {
		if (port_dissociate(evports_fd, PORT_SOURCE_FD, fd) != 0) {
			evports_panic_errno = errno;
			send_log(NULL, LOG_EMERG,
			    "port_dissociate failure: fd %d: %s\n",
			    fd, strerror(errno));
			abort();
		}
	} else {
		if (port_associate(evports_fd, PORT_SOURCE_FD, fd, events,
		    NULL) != 0) {
			evports_panic_errno = errno;
			send_log(NULL, LOG_EMERG,
			    "port_associate failure: fd %d: %s\n",
			    fd, strerror(errno));
			abort();
		}
	}
}

/*
 * Event Ports poller.  This routine interacts with the file descriptor
 * management data structures and routines; see the large block comment in
 * "src/fd.c" for more information.
 */
REGPRM2 static void evports_do_poll(struct poller *p, int exp)
{
	int i;
	int wait_time;
	struct timespec timeout;
	int r;
	int e = 0;
	unsigned int nevlist;
	int interrupted = 0;

	/*
	 * Scan the list of file descriptors with an updated status:
	 */
	for (i = 0; i < fd_nbupdt; i++) {
		int stateold, statenew;
		int fd = fd_updt[i];

		fdtab[fd].updated = 0;
		fdtab[fd].new = 0;

		if (fdtab[fd].owner == NULL)
			continue;

		stateold = fdtab[fd].state;
		statenew = fd_compute_new_polled_status(stateold);

		/*
		 * Check if the poll status has changed.  If it has, we need to
		 * reassociate now to update the event mask for this file
		 * descriptor.
		 */
		if ((stateold & FD_EV_POLLED_RW) !=
		    (statenew & FD_EV_POLLED_RW)) {
			int events = evports_state_to_events(statenew);

			evports_resync_fd(fd, events);

			fdtab[fd].state = statenew;
		}

		fd_alloc_or_release_cache_entry(fd, statenew);
	}
	fd_nbupdt = 0;

	/*
	 * Determine how long to wait for events to materialise on the port.
	 */
	if (fd_cache_num > 0 || run_queue > 0 || signal_queue_len > 0) {
		/*
		 * If there are other tasks ready to process, we don't want to
		 * sleep at all.
		 */
		wait_time = 0;
	} else if (exp == 0) {
		wait_time = MAX_DELAY_MS;
	} else if (tick_is_expired(exp, now_ms)) {
		wait_time = 0;
	} else {
		wait_time = TICKS_TO_MS(tick_remain(now_ms, exp)) + 1;
		if (wait_time > MAX_DELAY_MS) {
			wait_time = MAX_DELAY_MS;
		}
	}

	timeout.tv_sec = wait_time / 1000;
	timeout.tv_nsec = (wait_time % 1000) * 1000000;

	gettimeofday(&before_poll, NULL);
	nevlist = 1;
	if ((r = port_getn(evports_fd, evports_evlist, evports_evlist_max,
	    &nevlist, &timeout)) != 0) {
		switch (e = errno) {
		case ETIME:
			/*
			 * Though the manual page has not historically made it
			 * clear, port_getn() can return -1 with an errno of
			 * ETIME and still have returned some number of events.
			 */
			e = 0;
			r = 0;
			if (nevlist == 0)
				interrupted = 1;
			break;

		case EINTR:
			nevlist = 0;
			interrupted = 1;
			break;

		default:
			evports_panic_errno = e;
			send_log(NULL, LOG_EMERG,
			    "port_getn failure: fd %d: %s\n",
			    evports_fd, strerror(e));
			abort();
		}
	}
	tv_update_date(wait_time, interrupted);
	measure_idle();

	for (i = 0; i < nevlist; i++) {
		int fd = evports_evlist[i].portev_object;
		int events = evports_evlist[i].portev_events;
		int rebind_events;

		if (fdtab[fd].owner == NULL)
			continue;

		/*
		 * By virtue of receiving an event for this file descriptor, it
		 * is no longer associated with the port in question.  Store
		 * the previous event mask so that we may reassociate after
		 * processing is complete.
		 */
		rebind_events = evports_state_to_events(fdtab[fd].state);
		assert(rebind_events != 0);

		/*
		 * Clear all but the persistent poll bits (ERR & HUP):
		 */
		fdtab[fd].ev &= FD_POLL_STICKY;

		/*
		 * Set bits based on the events we received from the port:
		 */
		if (events & POLLIN)
			fdtab[fd].ev |= FD_POLL_IN;
		if (events & POLLOUT)
			fdtab[fd].ev |= FD_POLL_OUT;
		if (events & POLLERR)
			fdtab[fd].ev |= FD_POLL_ERR;
		if (events & POLLHUP)
			fdtab[fd].ev |= FD_POLL_HUP;

		/*
		 * Call connection processing callbacks.  Note that it's
		 * possible for this processing to alter the required event
		 * port assocation; i.e., the "state" member of the "fdtab"
		 * entry.  If it changes, the fd will be placed on the updated
		 * list for processing the next time we are called.
		 */
		fd_process_polled_events(fd);

		/*
		 * This file descriptor was closed during the processing of
		 * polled events.  No need to reassociate.
		 */
		if (fdtab[fd].owner == NULL)
			continue;

		/*
		 * Reassociate with the port, using the same event mask as
		 * before.  This call will not result in a dissociation as we
		 * asserted that _some_ events needed to be rebound above.
		 *
		 * Reassociating with the same mask allows us to mimic the
		 * level-triggered behaviour of poll(2).  In the event that we
		 * are interested in the same events on the next turn of the
		 * loop, this represents no extra work.
		 *
		 * If this additional port_associate(3C) call becomes a
		 * performance problem, we would need to verify that we can
		 * correctly interact with the file descriptor cache and update
		 * list (see "src/fd.c") to avoid reassociating here, or to use
		 * a different events mask.
		 */
		evports_resync_fd(fd, rebind_events);
	}
}

/*
 * Initialisation of the event ports poller.
 * Returns 0 in case of failure, non-zero in case of success.
 */
REGPRM1 static int evports_do_init(struct poller *p)
{
	p->private = NULL;

	evports_evlist_max = global.tune.maxpollevents;
	evports_evlist = calloc(evports_evlist_max, sizeof (port_event_t));
	if (evports_evlist == NULL) {
		goto fail;
	}

	if ((evports_fd = port_create()) == -1) {
		goto fail;
	}

	return 1;

fail:
	free(evports_evlist);
	evports_evlist = NULL;
	evports_evlist_max = 0;
	return 0;
}

/*
 * Termination of the event ports poller.
 * All resources are released and the poller is marked as inoperative.
 */
REGPRM1 static void evports_do_term(struct poller *p)
{
	if (evports_fd != -1) {
		assert(close(evports_fd) == 0);
		evports_fd = -1;
	}

	p->private = NULL;
	p->pref = 0;

	free(evports_evlist);
	evports_evlist = NULL;
	evports_evlist_max = 0;
}

/*
 * Run-time check to make sure we can allocate the resources needed for
 * the poller to function correctly.
 * Returns 1 on success, otherwise 0.
 */
REGPRM1 static int evports_do_test(struct poller *p)
{
	int fd;

	if ((fd = port_create()) == -1) {
		return 0;
	}

	assert(close(fd) == 0);
	return 1;
}

/*
 * Close and recreate the event port after fork().  Returns 1 on success,
 * otherwise 0.  If this function fails, "evports_do_term()" must be called to
 * clean up the poller.
 */
REGPRM1 static int evports_do_fork(struct poller *p)
{
	if (evports_fd != -1) {
		assert(close(evports_fd) == 0);
	}

	if ((evports_fd = port_create()) == -1) {
		return 0;
	}

	return 1;
}

/*
 * This constructor must be called before main() to register the event ports
 * poller.
 */
__attribute__((constructor))
static void evports_do_register(void)
{
	struct poller *p;

	if (nbpollers >= MAX_POLLERS)
		return;

	assert(evports_fd == -1);
	assert(evports_evlist == NULL);
	assert(evports_evlist_max == 0);

	p = &pollers[nbpollers++];

	p->name = "evports";
	p->pref = 300;
	p->private = NULL;

	p->init = evports_do_init;
	p->term = evports_do_term;
	p->test = evports_do_test;
	p->fork = evports_do_fork;
	p->poll = evports_do_poll;
}
