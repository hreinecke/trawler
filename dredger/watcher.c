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

pthread_mutex_t event_lock= PTHREAD_MUTEX_INITIALIZER;
LIST_HEAD(event_list);

enum migrate_direction {
	MIGRATE_OUT,
	MIGRATE_IN,
};

struct migrate_event {
	struct list_head next;
	pthread_t thr;
	pthread_mutex_t lock;
	struct backend *be;
	int fanotify_fd;
	char pathname[FILENAME_MAX];
	enum migrate_direction direction;
	int error;
	struct fanotify_event_metadata fa;
};

struct watcher_context {
	pthread_t thread;
	int fanotify_fd;
	struct backend *be;
};

void * unmigrate_file(void *arg);

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

struct migrate_event *malloc_migrate_event(struct backend *be, int fd)
{
	struct migrate_event *event;

	event = malloc(sizeof(struct migrate_event));
	if (!event) {
		err("watcher: cannot allocate event, error %d\n", errno);
		return NULL;
	}
	memset(event, 0, sizeof(struct migrate_event));
	INIT_LIST_HEAD(&event->next);
	pthread_mutex_init(&event->lock, NULL);
	event->fanotify_fd = fd;
	event->be = be;
	event->direction = MIGRATE_IN;
	return event;
}

void free_migrate_event(void *arg)
{
	struct migrate_event *event = arg;

	pthread_mutex_unlock(&event->lock);

	/* De-thread from list */
	if (!list_empty(&event->next)) {
		pthread_mutex_lock(&event_lock);
		list_del_init(&event->next);
		pthread_mutex_unlock(&event_lock);
	}

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
		event->fa.fd = 0;
	}
	pthread_mutex_destroy(&event->lock);
	free(event);
}

int migrate_file(struct backend *be, int fanotify_fd, char *filename)
{
	struct migrate_event *event, *tmp;
	int migrate_fd, src_fd;
	int ret;

	src_fd = open(filename, O_RDWR);
	if (src_fd < 0) {
		err("Cannot open source file '%s', error %d",
		    filename, errno);
		return errno;
	}
	event = malloc_migrate_event(be, fanotify_fd);
	if (!event) {
		err("Cannot allocate event, error %d", errno);
		return errno;
	}
	/*
	 * Check for concurrent events
	 */
	pthread_mutex_lock(&event_lock);
	list_for_each_entry(tmp, &event_list, next) {
		if (!strcmp(tmp->pathname, filename)) {
			ret = pthread_mutex_trylock(&tmp->lock);
			if (!ret) {
				/*
				 * Another event with the same pathname
				 * has finished. Don't try anything here.
				 */
				info("%s: found finished event, error %d",
				     filename, tmp->error);
				pthread_mutex_unlock(&tmp->lock);
				pthread_mutex_unlock(&event_lock);
				ret = EBUSY;
				goto out;
			} else if (ret == EBUSY) {
				/*
				 * Someone is working on it.
				 * Ignore if it's an unmigration,
				 * or wait for migration to complete.
				 */
				info("%s: found running %s event",
				     filename, tmp->direction == MIGRATE_OUT ?
				     "migrate" : "un-migrate");
				if (tmp->direction == MIGRATE_OUT) {
					pthread_mutex_lock(&tmp->lock);
					ret = tmp->error;
					pthread_mutex_unlock(&tmp->lock);
				}
				pthread_mutex_unlock(&event_lock);
				goto out;
			} else {
				/*
				 * Cannot get lock for whatever reason
				 * Abort migration
				 */
				info("%s: trylock for event failed, error %d",
				     filename, ret);
				pthread_mutex_unlock(&event_lock);
				goto out;
			}
		}
	}
	strcpy(event->pathname, filename);
	event->direction = MIGRATE_OUT;
	list_add(&event->next, &event_list);
	pthread_mutex_unlock(&event_lock);

	pthread_mutex_lock(&event->lock);
	migrate_fd = open_backend(event->be, event->pathname);
	if (migrate_fd < 0) {
		err("failed to open backend file %s, error %d",
		    event->pathname, errno);
		event->error = errno;
		goto out;
	}
	info("start migration on file '%s'", event->pathname);
	ret = migrate_backend(event->be, migrate_fd, src_fd);
	if (ret) {
		err("failed to migrate file %s, error %d",
		    event->pathname, ret);
		event->error = ret;
	} else {
		info("finished migration on file '%s'", event->pathname);
		event->error = 0;
	}
	close_backend(event->be, event->pathname, migrate_fd);
	pthread_mutex_unlock(&event->lock);
	if (!event->error) {
		ret = fanotify_mark(event->fanotify_fd, FAN_MARK_ADD,
				    FAN_ACCESS_PERM|FAN_EVENT_ON_CHILD,
				    AT_FDCWD, event->pathname);
		if (ret < 0) {
			err("failed to add fanotify mark "
			    "to %s, error %d\n", event->pathname, errno);
			unmigrate_file(event);
			ret = -ret;
		} else {
			info("Added fanotify mark on '%s'", event->pathname);
		}
	}
out:
	pthread_mutex_lock(&event->lock);
	free_migrate_event(event);
	return ret;
}

void * unmigrate_file(void *arg)
{
	struct migrate_event *event = arg;
	int migrate_fd, ret;

	pthread_mutex_lock(&event->lock);
	if (event->thr != (pthread_t)0)
		pthread_cleanup_push(free_migrate_event, (void *)event);
	migrate_fd = open_backend(event->be, event->pathname);
	if (!migrate_fd) {
		err("failed to open backend file %s, error %d",
		    event->pathname, errno);
		event->error = errno;
		goto out;
	}
	info("start un-migration on file '%s'", event->pathname);
	ret = unmigrate_backend(event->be, event->fa.fd, migrate_fd);
	if (ret < 0) {
		err("failed to unmigrate file %s, error %d",
			event->pathname, ret);
		event->error = ret;
	} else {
		info("finished un-migration on file '%s'", event->pathname);
		event->error = 0;
	}
	close_backend(event->be, event->pathname, migrate_fd);
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
	event = malloc_migrate_event(ctx->be, ctx->fanotify_fd);

	while (!daemon_stopped) {
		struct migrate_event *tmp;
		int rlen, ret, tmp_error;

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
		tmp_error = ESTALE;
		pthread_mutex_lock(&event_lock);
		list_for_each_entry(tmp, &event_list, next) {
			if (!strcmp(tmp->pathname, event->pathname)) {
				/* Check for concurrent events */
				dbg("concurrent event, dir %d error %d",
				    tmp->direction, event->error);
				pthread_mutex_lock(&tmp->lock);
				if (tmp->direction == MIGRATE_OUT)
					tmp_error = EAGAIN;
				else
					tmp_error = tmp->error;
				pthread_mutex_unlock(&tmp->lock);
				break;
			}
		}
		if (!tmp_error) {
			/* Previous migration done, continue */
			pthread_mutex_unlock(&event_lock);
			continue;
		}
		/*
		 * Start unmigration when
		 * - Previous unmigration failed
		 * or
		 * - Previous migration finished
		 * or
		 * - No previous migration attempted
		 */
		list_add(&event->next, &event_list);
		pthread_mutex_unlock(&event_lock);
		ret = pthread_create(&event->thr, NULL, unmigrate_file, event);
		if (ret < 0) {
			err("Failed to start thread, error %d", errno);
			pthread_mutex_lock(&event->lock);
			free_migrate_event(event);
		}
		event = malloc_migrate_event(ctx->be, ctx->fanotify_fd);
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

int check_watcher(char *pathname)
{
	struct migrate_event *event = NULL, *tmp;
	int ret;

	pthread_mutex_lock(&event_lock);
	list_for_each_entry(tmp, &event_list, next) {
		if (!strcmp(tmp->pathname, pathname)) {
			ret = pthread_mutex_trylock(&event->lock);
			if (!ret) {
				ret = event->error;
				pthread_mutex_unlock(&event->lock);
			}
			break;
		}
	}
	pthread_mutex_unlock(&event_lock);
	return ret;
}
