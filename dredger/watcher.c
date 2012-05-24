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
#include "fanotify-mark-syscall.h"
#include "list.h"
#include "logging.h"
#include "dredger.h"
#include "backend.h"

#define LOG_AREA "watcher"

struct migrate_event {
	pthread_t thr;
	struct backend *be;
	int fanotify_fd;
	char pathname[FILENAME_MAX];
	int error;
	struct fanotify_event_metadata fa;
};

struct watcher_context {
	pthread_t thread;
	int fanotify_fd;
	struct backend *be;
	struct migrate_event *event;
};

int get_fname(int fd, char *fname)
{
	int len;
	char buf[FILENAME_MAX];

	sprintf(buf, "/proc/self/fd/%d", fd); /* link to local path name */
	len = readlink(buf, fname, FILENAME_MAX-1);
	if (len <= 0) {
		fname[0] = '\0';
	} else {
		fname[len] = '\0';
	}
	info("read filename '%s' len '%d'", fname, len);
	return len;
}

struct migrate_event *malloc_migrate_event(struct backend *be, int fd)
{
	struct migrate_event *event;

	event = malloc(sizeof(struct migrate_event));
	if (!event) {
		err("watcher: cannot allocate event, error %d\n", errno);
		return NULL;
	}
	memset(event, 0, sizeof(struct migrate_event));
	event->fanotify_fd = fd;
	event->be = be;
	return event;
}

void cleanup_migrate_event(struct migrate_event *event)
{
	if (event->fa.mask & FAN_ACCESS_PERM) {
		struct fanotify_response resp;

		resp.fd = event->fa.fd;
		if (event->error)
			resp.response = FAN_DENY;
		else
			resp.response = FAN_ALLOW;
		if (write(event->fanotify_fd, &resp, sizeof(resp)) < 0) {
			err("watcher: Failed to write fanotify "
			    "response: error %d", errno);
		} else {
			dbg("watcher: Wrote response '%s'",
			       (resp.response == FAN_ALLOW) ?
			       "FAN_ALLOW" : "FAN_DENY");
		}
	}
	if (event->fa.fd >= 0) {
		close(event->fa.fd);
		event->fa.fd = -1;
	}
	memset(event->pathname, 0, sizeof(event->pathname));
}

void cleanup_unmigrate_file(void *arg)
{
	struct migrate_event *event = arg;

	event->thr = (pthread_t)0;
	cleanup_migrate_event(event);
	free(event);
}

void * unmigrate_file(void *arg)
{
	struct migrate_event *event = arg;
	int migrate_fd, ret;

	if (event->thr != (pthread_t)0)
		pthread_cleanup_push(cleanup_unmigrate_file, (void *)event);
	migrate_fd = open_backend(event->be, event->pathname);
	if (!migrate_fd) {
		if (errno == ENOENT) {
			info("backend file %s already un-migrated",
			     event->pathname);
			event->error = 0;
		} else {
			err("failed to open backend file %s, error %d",
			    event->pathname, errno);
			event->error = errno;
			goto out;
		}
	} else {
		info("start un-migration on file '%s'", event->pathname);
		ret = unmigrate_backend(event->be, event->fa.fd, migrate_fd);
		if (ret < 0) {
			err("failed to unmigrate file %s, error %d",
			    event->pathname, ret);
			event->error = ret;
		} else {
			info("finished un-migration on file '%s'",
			     event->pathname);
			event->error = 0;
		}
		close_backend(event->be, event->pathname, migrate_fd);
	}
	if (!event->error) {
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

void cleanup_context(void * arg)
{
	struct watcher_context *ctx = arg;

	if (ctx->event) {
		cleanup_migrate_event(ctx->event);
		free(ctx->event);
		ctx->event = NULL;
	}
	free(ctx);
}

void * watch_fanotify(void * arg)
{
	struct watcher_context *ctx = arg;
	fd_set rfd;
	struct timeval tmo;
	int i = 0;
	struct migrate_event *event;

	pthread_cleanup_push(cleanup_context, ctx);
	ctx->event = malloc_migrate_event(ctx->be, ctx->fanotify_fd);

	while (!daemon_stopped) {
		int rlen, ret;

		FD_ZERO(&rfd);
		FD_SET(ctx->fanotify_fd, &rfd);
		tmo.tv_sec = 5;
		tmo.tv_usec = 0;
		ret = select(ctx->fanotify_fd + 1, &rfd, NULL, NULL, &tmo);
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
		if (!FD_ISSET(ctx->fanotify_fd, &rfd)) {
			err("select returned for invalid fd");
			continue;
		}
		event = ctx->event;
		rlen = read(ctx->fanotify_fd, &event->fa,
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
		ctx->event = NULL;
		ret = pthread_create(&event->thr, NULL, unmigrate_file,
				     event);
		if (ret < 0) {
			err("Failed to start thread, error %d", errno);
			cleanup_migrate_event(event);
			ctx->event = event;
		} else
			ctx->event = malloc_migrate_event(ctx->be,
							  ctx->fanotify_fd);
		i++;
	}
	pthread_cleanup_pop(1);
	return NULL;
}

pthread_t start_watcher(struct backend *be, int fanotify_fd)
{
	int retval;
	struct watcher_context *ctx;

	ctx = malloc(sizeof(struct watcher_context));
	if (!ctx) {
		err("Failed to allocate watcher context");
		return (pthread_t)0;
	}
	ctx->be = be;
	ctx->fanotify_fd = fanotify_fd;

	retval = pthread_create(&ctx->thread, NULL,
				watch_fanotify, ctx);
	if (retval) {
		err("Failed to start fanotify watcher, error %d", retval);
		free(ctx);
		errno = retval;
		return (pthread_t)0;
	}
	info("Started fanotify watcher");

	return ctx->thread;
}

int stop_watcher(pthread_t watcher_thr)
{
	pthread_cancel(watcher_thr);
	pthread_join(watcher_thr, NULL);
	info("Stopped fanotify watcher");
	return 0;
}
