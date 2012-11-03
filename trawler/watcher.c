/*
 * watcher.c
 *
 * Inotify-based directory watcher for trawler.
 * Copyright (c) 2012 Hannes Reinecke <hare@suse.de>
 */
#include <stdio.h>
#include <sys/types.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <limits.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <search.h>
#include <pthread.h>

#include "logging.h"

#define LOG_AREA "watcher"

struct event_watch {
	int ew_wd;
	char ew_path[PATH_MAX];
};

void *watch_tree = NULL;
static int stopped;
pthread_t watcher_thr;
int inotify_fd;
pthread_mutex_t tree_mutex = PTHREAD_MUTEX_INITIALIZER;

int compare_wd(const void *a, const void *b)
{
	const struct event_watch *ew1 = a, *ew2 = b;

	if (ew1->ew_wd < ew2->ew_wd)
		return -1;
	if (ew1->ew_wd > ew2->ew_wd)
		return 1;
	return 0;
}

int compare_path(const void *a, const void *b)
{
	const struct event_watch *ew1 = a, *ew2 = b;

	return strcmp(ew1->ew_path, ew2->ew_path);
}

void free_watch(void *p)
{
	struct event_watch *ew = p;

	inotify_rm_watch(inotify_fd, ew->ew_wd);
	info("%s: removed inotify watch %d\n",
	     ew->ew_path, ew->ew_wd);
	free(ew);
}

int insert_inotify(char *dirname, int locked)
{
	struct event_watch *ew;
	void *val;

	ew = malloc(sizeof(struct event_watch));
	if (!ew) {
		err("%s: cannot allocate watch entry", dirname);
		return -ENOMEM;
	}
	ew->ew_wd = inotify_add_watch(inotify_fd, dirname, IN_ALL_EVENTS);
	if (ew->ew_wd < 0) {
		err("%s: inotify_add_watch failed with %d", dirname, errno);
		free(ew);
		return -errno;
	}
	strcpy(ew->ew_path, dirname);
	if (!locked)
		pthread_mutex_lock(&tree_mutex);
	val = tsearch((void *)ew, &watch_tree, compare_wd);
	if (!locked)
		pthread_mutex_unlock(&tree_mutex);
	if (!val) {
		err("%s: Failed to insert watch entry", dirname);
		inotify_rm_watch(inotify_fd, ew->ew_wd);
		free(ew);
		return -ENOMEM;
	} else if ((*(struct event_watch **) val) != ew) {
		err("%s: watch %d already present", dirname, ew->ew_wd);
		free(ew);
		return -EEXIST;
	}
	info("%s: added inotify watch %d %p", ew->ew_path, ew->ew_wd, ew);
	return 0;
}

int remove_inotify(char *dirname, int locked)
{
	void *val;
	struct event_watch ew, *found_ew;

	strcpy(ew.ew_path, dirname);
	if (!locked)
		pthread_mutex_lock(&tree_mutex);
	val = tfind((void *)&ew, &watch_tree, compare_path);
	if (!locked)
		pthread_mutex_unlock(&tree_mutex);
	if (!val) {
		err("%s: watch entry not found in tree", dirname);
		return -EINVAL;
	}
	found_ew = *(struct event_watch **)val;
	if (!locked)
		pthread_mutex_lock(&tree_mutex);
	val = tdelete((void *)found_ew, &watch_tree, compare_wd);
	if (!locked)
		pthread_mutex_unlock(&tree_mutex);
	if (!val) {
		err("%s: failed to remove in watch entry", dirname);
		return -EINVAL;
	}
	inotify_rm_watch(inotify_fd, found_ew->ew_wd);
	info("%s: removed inotify watch %d",
	       found_ew->ew_path, found_ew->ew_wd);
	free(found_ew);

	return 0;
}

#define EVENT_SIZE (sizeof(struct inotify_event))
#define BUF_LEN (1024 * (EVENT_SIZE + 16))

void release_tree_lock(void *arg)
{
	pthread_mutex_unlock(&tree_mutex);
}

void * watch_dir(void * arg)
{
	int inotify_fd = *(int *)arg;
	fd_set rfd;
	struct timeval tmo;
	char buf[BUF_LEN];

	while (!stopped) {
		int rlen, ret, i = 0;

		FD_ZERO(&rfd);
		FD_SET(inotify_fd, &rfd);
		tmo.tv_sec = 5;
		tmo.tv_usec = 0;
		ret = select(inotify_fd + 1, &rfd, NULL, NULL, &tmo);
		if (ret < 0) {
			if (ret == EINTR)
				continue;
			err("select returned %d\n", errno);
			break;
		}
		if (ret == 0) {
			err( "select timeout\n");
			continue;
		}
		if (!FD_ISSET(inotify_fd, &rfd)) {
			err( "select returned for invalid fd\n");
			continue;
		}

		rlen = read(inotify_fd, buf, BUF_LEN);
		while (i < rlen) {
			struct inotify_event *in_ev;
			const char *type;
			void *val;
			struct event_watch ew, *found_ew;
			const char *op;
			char path[PATH_MAX];

			in_ev = (struct inotify_event *)&(buf[i]);

			if (!in_ev->len)
				goto next;

			ew.ew_wd = in_ev->wd;
			pthread_cleanup_push(release_tree_lock, NULL);
			pthread_mutex_lock(&tree_mutex);
			val = tfind((void *)&ew, &watch_tree,
					 compare_wd);
			pthread_cleanup_pop(1);
			if (!val) {
				err( "inotify event %d not found "
					"in tree", in_ev->wd);
				goto next;
			}
			found_ew = *(struct event_watch **)val;
			if (in_ev->mask & IN_ISDIR) {
				type = "dir";
			} else {
				type = "file";
			}
			if (in_ev->mask & IN_IGNORED) {
				info("inotify event %d removed",
				     in_ev->wd);
				goto next;
			}
			if (in_ev->mask & IN_Q_OVERFLOW) {
				info("inotify event %d: queue overflow",
				     in_ev->wd);
				goto next;
			}
			info("event %d: %x",
			     in_ev->wd, in_ev->mask);
			if (in_ev->mask & IN_CREATE)
				op = "created";
			else if (in_ev->mask & IN_DELETE)
				op = "deleted";
			else if (in_ev->mask & IN_MODIFY)
				op = "modified";
			else if (in_ev->mask & IN_OPEN)
				op = "opened";
			else if (in_ev->mask & IN_CLOSE)
				op = "closed";
			else if (in_ev->mask & IN_MOVE)
				op = "moved";
			else
				op = "<unhandled>";
			sprintf(path, "%s/%s", found_ew->ew_path, in_ev->name);
			info("\t%s %s %s", op, type, path);
			if (in_ev->mask & IN_ISDIR) {
				pthread_cleanup_push(release_tree_lock, NULL);
				pthread_mutex_lock(&tree_mutex);
				if ((in_ev->mask & IN_DELETE) ||
				    (in_ev->mask & IN_MOVED_FROM))
					remove_inotify(path, 1);
				if ((in_ev->mask & IN_CREATE) ||
				    (in_ev->mask & IN_MOVED_TO))
					insert_inotify(path, 1);
				pthread_cleanup_pop(1);
			}
		next:
			i += EVENT_SIZE + in_ev->len;
		}
	}

	return NULL;
}

int start_watcher(void)
{
	int retval;

	stopped = 0;

	inotify_fd = inotify_init();
	if (inotify_fd < 0) {
		err("Failed to initialize inotify, error %d", errno);
		return errno;
	}

	retval = pthread_create(&watcher_thr, NULL, watch_dir, &inotify_fd);
	if (retval) {
		err("Failed to create watcher thread: %d", retval);
	}
	info("Starting inotify watcher\n");
	return retval;
}

int stop_watcher(void)
{
	stopped = 1;
	pthread_cancel(watcher_thr);
	pthread_join(watcher_thr, NULL);
	info("Stopped inotify watcher");
	pthread_mutex_lock(&tree_mutex);
	tdestroy(watch_tree, free_watch);
	pthread_mutex_unlock(&tree_mutex);
	return 0;
}
