/*
 * watcher.c
 *
 * fanotify-based watcher for dredger.
 * Copyright (c) 2012 Hannes Reinecke <hare@suse.de>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <sys/syslog.h>
#include <stdarg.h>
#include "fanotify.h"
#include "fanotify-syscall.h"
#include "list.h"
#include "logging.h"
#include "dredger.h"

pthread_mutex_t event_lock= PTHREAD_MUTEX_INITIALIZER;
LIST_HEAD(event_list);

enum migrate_state {
	MIGRATE_NONE,
	MIGRATE_STARTED,
	MIGRATE_FAILED,
	MIGRATE_DONE,
	MIGRATE_ABORTED,
	UNMIGRATE_STARTED,
	UNMIGRATE_OPEN,
	UNMIGRATE_BUSY,
	UNMIGRATE_FAILED,
	UNMIGRATE_DONE,
	UNMIGRATE_CLOSE
};

struct migrate_event {
	struct list_head next;
	pthread_t thr;
	int fanotify_fd;
	char pathname[FILENAME_MAX];
	enum migrate_state state;
	struct fanotify_event_metadata fa;
};

int get_fname(int fd, char *fname)
{
	int len;
	char buf[FILENAME_MAX];

	sprintf(buf, "/proc/self/fd/%d", fd); /* link to local path name */
	len = readlink(buf, fname, FILENAME_MAX-1);
	if (len <= 0) {
		fname[0] = '\0';
	}
	return len;
}

struct migrate_event *malloc_migrate_event(int fd)
{
	struct migrate_event *event;

	event = malloc(sizeof(struct migrate_event));
	if (!event) {
		err("watcher: cannot allocate event, error %d\n", errno);
		return NULL;
	}
	memset(event, 0, sizeof(struct migrate_event));
	INIT_LIST_HEAD(&event->next);
	event->fanotify_fd = fd;
	return event;
}

void free_migrate_event(void *arg)
{
	struct migrate_event *event = arg;

	pthread_mutex_lock(&event_lock);
	list_del_init(&event->next);
	pthread_mutex_unlock(&event_lock);
	if (event->fa.mask & FAN_ACCESS_PERM) {
		struct fanotify_response resp;

		resp.fd = event->fa.fd;
		if (event->state == UNMIGRATE_BUSY ||
		    event->state == UNMIGRATE_FAILED)
			resp.response = FAN_DENY;
		else
			resp.response = FAN_ALLOW;
		if (write(event->fanotify_fd, &resp, sizeof(resp)) < 0) {
			err("watcher: Failed to write fanotify "
			    "response: error %d\n", errno);
		} else {
			dbg("watcher: Wrote response '%s'\n",
			       (resp.response == FAN_ALLOW) ?
			       "FAN_ALLOW" : "FAN_DENY");
		}
	}
	if (event->fa.fd >= 0) {
		close(event->fa.fd);
		event->fa.fd = 0;
	}
	free(event);
}

int migrate_file(int fanotify_fd, char *filename)
{
	struct migrate_event *event, *tmp;
	int migrate_fd;
	enum migrate_state state;
	int ret;

	event = malloc_migrate_event(fanotify_fd);
	if (!event) {
		err("Cannot allocate event, error %d", errno);
		return errno;
	}
	/* We have checked for this event prior to entry
	 * So it would be rather rare event if we've
	 * found it now.
	 */
	pthread_mutex_lock(&event_lock);
	list_for_each_entry(tmp, &event_list, next) {
		if (!strcmp(tmp->pathname, filename)) {
			/* Hmph. Abort migration */
			free(event);
			pthread_mutex_unlock(&event_lock);
			return EBUSY;
		}
	}
	strcpy(event->pathname, filename);
	event->state = MIGRATE_STARTED;
	list_add(&event->next, &event_list);
	pthread_mutex_unlock(&event_lock);

	migrate_fd = open_backend_file(event->pathname);
	if (migrate_fd < 0) {
		err("failed to open backend file %s, error %d",
		    event->pathname, errno);
		ret = errno;
		goto out;
	}
	pthread_mutex_lock(&event_lock);
	state = event->state;
	pthread_mutex_unlock(&event_lock);
	if (state == MIGRATE_ABORTED) {
		info("migration on file %s aborted",
		     event->pathname);
		close_backend_file(migrate_fd);
		ret = EINTR;
		goto out;
	}
	ret = migrate_backend_file(event->fa.fd, migrate_fd);
	pthread_mutex_lock(&event_lock);
	if (event->state == MIGRATE_ABORTED) {
		info("migration on file %s aborted", event->pathname);
	} else if (ret) {
		err("failed to migrate file %s, error %d",
		    event->pathname, ret);
		event->state = MIGRATE_FAILED;
	} else {
		event->state = MIGRATE_DONE;
	}
	pthread_mutex_unlock(&event_lock);
	close_backend_file(migrate_fd);
	if (event->state == MIGRATE_DONE) {
		ret = fanotify_mark(event->fanotify_fd, FAN_MARK_ADD,
				    FAN_ACCESS_PERM|FAN_EVENT_ON_CHILD,
				    AT_FDCWD, event->pathname);
		if (ret < 0) {
			err("failed to add fanotify mark "
			    "to %s, error %d\n", event->pathname, errno);
		}
		ret = -ret;
	}
out:
	pthread_mutex_lock(&event_lock);
	list_del_init(&event->next);
	pthread_mutex_unlock(&event_lock);
	free(event);
	return ret;
}

void * unmigrate_file(void *arg)
{
	struct migrate_event *event = arg;
	int migrate_fd, ret;

	pthread_cleanup_push(free_migrate_event, (void *)event);
	event->state = UNMIGRATE_OPEN;
	migrate_fd = open_backend_file(event->pathname);
	if (!migrate_fd) {
		err("failed to open backend file %s, error %d",
		    event->pathname, errno);
		goto out;
	}
	event->state = UNMIGRATE_BUSY;
	ret = unmigrate_backend_file(event->fa.fd, migrate_fd);
	if (ret < 0) {
		err("failed to unmigrate file %s, error %d",
			event->pathname, ret);
		event->state = UNMIGRATE_FAILED;
	} else {
		event->state = UNMIGRATE_DONE;
	}
	close_backend_file(migrate_fd);
	if (event->state == UNMIGRATE_DONE) {
		ret = fanotify_mark(event->fanotify_fd, FAN_MARK_REMOVE,
				    FAN_ACCESS_PERM|FAN_EVENT_ON_CHILD,
				    AT_FDCWD, event->pathname);
		if (ret < 0) {
			err("failed to remove fanotify mark "
			    "from %s, error %d", event->pathname, errno);
		}
	}
out:
	pthread_cleanup_pop(1);
	return NULL;
}

void * watch_fanotify(void * arg)
{
	int fanotify_fd = *(int *)arg;
	fd_set rfd;
	struct timeval tmo;
	int i = 0;
	struct migrate_event *event;

	event = malloc_migrate_event(fanotify_fd);

	while (!daemon_stopped) {
		struct migrate_event *tmp;
		int rlen, ret;

		FD_ZERO(&rfd);
		FD_SET(fanotify_fd, &rfd);
		tmo.tv_sec = 5;
		tmo.tv_usec = 0;
		ret = select(fanotify_fd + 1, &rfd, NULL, NULL, &tmo);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			err("select returned %d", errno);
			break;
		}
		if (ret == 0) {
			dbg("watcher: select timeout");
			continue;
		}
		if (!FD_ISSET(fanotify_fd, &rfd)) {
			err("select returned for invalid fd");
			continue;
		}

		rlen = read(fanotify_fd, &event->fa,
			    sizeof(struct fanotify_event_metadata));
		if (rlen < 0) {
			err("error %d on reading fanotify event", errno);
			continue;
		}
		if (!(event->fa.mask & FAN_ACCESS_PERM)) {
			continue;
		}

		if (get_fname(event->fa.fd, event->pathname) < 0) {
			err("cannot retrieve filename");
			continue;
		}

		dbg("fanotify event %d: mask 0x%02lX, fd %d (%s), pid %d",
		       i, (unsigned long) event->fa.mask, event->fa.fd,
		       event->pathname, event->fa.pid);
		pthread_mutex_lock(&event_lock);
		list_for_each_entry(tmp, &event_list, next) {
			if (!strcmp(tmp->pathname, event->pathname)) {
				if (tmp->state == MIGRATE_STARTED) {
					/* Migration in progress. Abort */
					tmp->state = MIGRATE_ABORTED;
					continue;
				}
				if (tmp->state == MIGRATE_DONE ||
				    tmp->state == MIGRATE_FAILED)
					/* Migration done, continue */
					continue;
				event->state = UNMIGRATE_BUSY;
				break;
			}
		}
		if (event->state != UNMIGRATE_BUSY)
			list_add(&event->next, &event_list);
		pthread_mutex_unlock(&event_lock);
		if (list_empty(&event->next))
			continue;
		ret = pthread_create(&event->thr, NULL, unmigrate_file, event);
		if (ret < 0) {
			err("Failed to start thread, error %d", errno);
			free_migrate_event(event);
		}
		event = malloc_migrate_event(fanotify_fd);
		i++;
	}
	return NULL;
}

pthread_t start_watcher(void)
{
	pthread_t watcher_thr;
	int retval, fanotify_fd;

	retval = pthread_create(&watcher_thr, NULL,
				watch_fanotify, &fanotify_fd);
	if (retval) {
		err("Failed to start fanotify watcher, error %d", retval);
		errno = retval;
		return (pthread_t)0;
	}
	info("Started fanotify watcher");

	return watcher_thr;
}

int stop_watcher(pthread_t watcher_thr)
{
	pthread_cancel(watcher_thr);
	pthread_join(watcher_thr, NULL);
	info("Stopped fanotify watcher");
	return 0;
}

int check_watcher(char *pathname)
{
	struct migrate_event *event;
	enum migrate_state state = MIGRATE_NONE;
	int ret;

	pthread_mutex_lock(&event_lock);
	list_for_each_entry(event, &event_list, next) {
		if (!strcmp(event->pathname, pathname)) {
			state = event->state;
			break;
		}
	}
	pthread_mutex_unlock(&event_lock);
	switch (state) {
	case MIGRATE_NONE:
		ret = 0;
		break;
	case UNMIGRATE_FAILED:
		ret = EAGAIN;
		break;
	default:
		ret = EBUSY;
		break;
	}
	return ret;
}
