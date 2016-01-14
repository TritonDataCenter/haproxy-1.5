/*
 * FD polling functions for illumos Event Ports
 *
 * Copyright 2000-2014 Willy Tarreau <w@1wt.eu>
 * Copyright 2016 Joyent, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>

#include <poll.h>
#include <port.h>
#include <errno.h>
#include <assert.h>
#include <syslog.h>

#include <common/compat.h>
#include <common/config.h>
#include <common/ticks.h>
#include <common/time.h>

#include <types/global.h>

#include <proto/fd.h>
#include <proto/signal.h>
#include <proto/task.h>


/* private data */
static int evports_fd = -1;
static port_event_t *evports_evlist;
static int evports_evlist_max = 0;

/*
 * Convert the "state" member of "fdtab" into an event ports event mask.
 */
static int
evports_state_to_events(int state)
{
	int events = 0;

	if (state & FD_EV_POLLED_W)
		events |= POLLOUT;
	if (state & FD_EV_POLLED_R)
		events |= POLLIN;

	return (events);
}

static void
evports_resync_fd(int fd, int events)
{
	if (events == 0) {
		if (port_dissociate(evports_fd, PORT_SOURCE_FD, fd) != 0) {
			send_log(NULL, LOG_EMERG, "port_dissociate failure: fd %d: %s\n", fd, strerror(errno));
			if (errno != ENOENT) {
				abort();
			}
		}
	} else {
		if (port_associate(evports_fd, PORT_SOURCE_FD, fd, events, NULL) != 0) {
			send_log(NULL, LOG_EMERG, "port_associate failure: fd %d: %s\n", fd, strerror(errno));
			abort();
		}
	}
}

/*
 * Event Ports poller
 */
REGPRM2 static void evports_do_poll(struct poller *p, int exp)
{
	int i;
	int wait_time;
	struct timespec timeout;
	int r;
	int e = 0;
	unsigned int nevlist = 0;

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
		 * reassociate now.
		 */
		if ((stateold & FD_EV_POLLED_RW) != (statenew & FD_EV_POLLED_RW)) {
			int events = evports_state_to_events(statenew);

			evports_resync_fd(fd, events);

			fdtab[fd].state = statenew;
		}

		fd_alloc_or_release_cache_entry(fd, statenew);
	}
	fd_nbupdt = 0;

	/* now let's wait for events */
	if (fd_cache_num || run_queue || signal_queue_len)
		wait_time = 0;
	else if (!exp)
		wait_time = MAX_DELAY_MS;
	else if (tick_is_expired(exp, now_ms))
		wait_time = 0;
	else {
		wait_time = TICKS_TO_MS(tick_remain(now_ms, exp)) + 1;
		if (wait_time > MAX_DELAY_MS)
			wait_time = MAX_DELAY_MS;
	}

	timeout.tv_sec = wait_time / 1000;
	timeout.tv_nsec = (wait_time % 1000) * 1000000;

	gettimeofday(&before_poll, NULL);
	nevlist = 1;
	if ((r = port_getn(evports_fd, evports_evlist, evports_evlist_max,
	    &nevlist, &timeout)) != 0) {
		/*
		 * XXX lift dap's comment
		 */
		if ((e = errno) == ETIME) {
			e = r = 0;
		} else {
			assert(e == EINTR);
		}
	}
	tv_update_date(wait_time, (r == 0));
	measure_idle();

	if (r != 0)
		return;

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
		 * Call connection processing callbacks:
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
		 * before:
		 */
		evports_resync_fd(fd, rebind_events);
	}
}

/*
 * Initialization of the poll() poller.
 * Returns 0 in case of failure, non-zero in case of success. If it fails, it
 * disables the poller by setting its pref to 0.
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
 * Termination of the poll() poller.
 * Memory is released and the poller is marked as unselectable.
 */
REGPRM1 static void evports_do_term(struct poller *p)
{
	if (evports_fd != -1) {
		close(evports_fd);
		evports_fd = -1;
	}

	p->private = NULL;
	p->pref = 0;

	free(evports_evlist);
	evports_evlist = NULL;
	evports_evlist_max = 0;
}

/*
 * Check that the poller works.
 * Returns 1 if OK, otherwise 0.
 */
REGPRM1 static int evports_do_test(struct poller *p)
{
	return 1;
}

REGPRM1 static int evports_do_fork(struct poller *p)
{
	if (evports_fd != -1) {
		close(evports_fd);
	}
	if ((evports_fd = port_create()) == -1) {
		return 0;
	}
	return 1;
}

/*
 * It is a constructor, which means that it will automatically be called before
 * main(). This is GCC-specific but it works at least since 2.95.
 * Special care must be taken so that it does not need any uninitialized data.
 */
__attribute__((constructor))
static void evports_do_register(void)
{
	struct poller *p;

	if (nbpollers >= MAX_POLLERS)
		return;

	evports_fd = -1;
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


/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
