/*
 * watcher.c
 *
 * fanotify-based watcher for dredger.
 * Copyright (c) 2012 Hannes Reinecke <hare@suse.de>
 */
#include <stdio.h>
#include <stdlib.h>
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
	MIGRATE_NONE = -1,
	MIGRATE_WATCHED,
	MIGRATE_STARTED,
	MIGRATE_OPEN,
	MIGRATE_BUSY,
	MIGRATE_FAILED,
	MIGRATE_DONE,
	MIGRATE_CLOSE
};

struct unmigrate_event {
	struct list_head next;
	pthread_t thr;
	int fanotify_fd;
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

struct unmigrate_event *malloc_unmigrate_event(int fd)
{
	struct unmigrate_event *event;

	event = malloc(sizeof(struct unmigrate_event));
	if (!event) {
		fprintf(stderr, "cannot allocate event, error %d\n",
			errno);
		return NULL;
	}
	memset(event, 0, sizeof(struct unmigrate_event));
	INIT_LIST_HEAD(&event->next);
	event->fanotify_fd = fd;
	return event;
}

void free_unmigrate_event(void *arg)
{
	struct unmigrate_event *event = arg;

	pthread_mutex_lock(&event_lock);
	list_del_init(&event->next);
	pthread_mutex_unlock(&event_lock);
	if (event->fa.mask & FAN_ACCESS_PERM) {
		struct fanotify_response resp;

		resp.fd = event->fa.fd;
		if (event->state == MIGRATE_BUSY ||
		    event->state == MIGRATE_FAILED)
			resp.response = FAN_DENY;
		else
			resp.response = FAN_ALLOW;
		if (write(event->fanotify_fd, &resp, sizeof(resp)) < 0) {
			fprintf(stderr, "Failed to write fanotify "
				"response: error %d\n", errno);
		} else {
			printf("Wrote response '%s'\n",
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

void * unmigrate_file(void *arg)
{
	struct unmigrate_event *event = arg;
	int migrate_fd, ret;
	char fname[PATH_MAX];

	pthread_cleanup_push(free_unmigrate_event, (void *)event);
	if (!(event->fa.mask & FAN_ACCESS_PERM)) {
		goto out;
	}

	event->state = MIGRATE_STARTED;
	if (get_fname(event->fa.fd, fname) < 0) {
		fprintf(stderr, "cannot retrieve filename\n");
		goto out;
	}

	event->state = MIGRATE_OPEN;
	migrate_fd = open_backend_file(fname);
	if (!migrate_fd) {
		fprintf(stderr,"failed to open backend file %s, error %d\n",
			fname, errno);
		goto out;
	}
	event->state = MIGRATE_BUSY;
	ret = unmigrate_backend_file(event->fa.fd, migrate_fd);
	if (ret < 0) {
		fprintf(stderr, "failed to unmigrate file %s, error %d\n",
			fname, ret);
		event->state = MIGRATE_FAILED;
	} else {
		event->state = MIGRATE_DONE;
	}
	close_backend_file(migrate_fd);
	if (event->state == MIGRATE_DONE) {
		ret = fanotify_mark(event->fanotify_fd, FAN_MARK_REMOVE,
				    FAN_ACCESS_PERM|FAN_EVENT_ON_CHILD,
				    AT_FDCWD, fname);
		if (ret < 0) {
			fprintf(stderr, "failed to remove fanotify mark "
				"from %s, error %d\n", fname, errno);
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
	struct unmigrate_event *event;

	event = malloc_unmigrate_event(fanotify_fd);

	while (!daemon_stopped) {
		int rlen, ret;

		FD_ZERO(&rfd);
		FD_SET(fanotify_fd, &rfd);
		tmo.tv_sec = 5;
		tmo.tv_usec = 0;
		ret = select(fanotify_fd + 1, &rfd, NULL, NULL, &tmo);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr,"select returned %d\n", errno);
			break;
		}
		if (ret == 0) {
			fprintf(stderr, "select timeout\n");
			continue;
		}
		if (!FD_ISSET(fanotify_fd, &rfd)) {
			fprintf(stderr, "select returned for invalid fd\n");
			continue;
		}

		rlen = read(fanotify_fd, &event->fa,
			    sizeof(struct fanotify_event_metadata));
		if (rlen < 0) {
			fprintf(stderr, "error %d on reading fanotify\n",
				errno);
			continue;
		}
		printf("fanotify event %d: mask 0x%02lX, fd %d, pid %d\n",
		       i, (unsigned long) event->fa.mask, event->fa.fd,
		       event->fa.pid);
		pthread_mutex_lock(&event_lock);
		list_add(&event->next, &event_list);
		pthread_mutex_unlock(&event_lock);
		ret = pthread_create(&event->thr, NULL, unmigrate_file, event);
		if (ret < 0) {
			fprintf(stderr, "Failed to start thread, error %d\n",
				errno);
			free_unmigrate_event(event);
		}
		event = malloc_unmigrate_event(fanotify_fd);
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
		fprintf(stderr, "Failed to start fanotify watcher, error %d\n",
			retval);
		errno = retval;
		return NULL;
	}
	printf("Started fanotify watcher\n");

	return watcher_thr;
}

int stop_watcher(pthread_t watcher_thr)
{
	pthread_cancel(watcher_thr);
	pthread_join(watcher_thr, NULL);
	printf("Stopped fanotify watcher\n");
	return 0;
}

