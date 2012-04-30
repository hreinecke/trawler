/*
 * dredger.c
 *
 * Main routine for dredger.
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
#include "fanotify.h"
#include "fanotify-syscall.h"
#include "list.h"

pthread_cond_t exit_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t exit_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t event_lock= PTHREAD_MUTEX_INITIALIZER;

LIST_HEAD(event_list);

enum migrate_state {
	MIGRATE_NONE,
	MIGRATE_INIT,
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

int stopped;
char file_backend_prefix[FILENAME_MAX];

static void *
signal_set(int signo, void (*func) (int))
{
	int r;
	struct sigaction sig;
	struct sigaction osig;

	sig.sa_handler = func;
	sigemptyset(&sig.sa_mask);
	sig.sa_flags = 0;

	r = sigaction(signo, &sig, &osig);

	if (r < 0)
		return (SIG_ERR);
	else
		return (osig.sa_handler);
}

static void sigend(int sig)
{
	stopped = 1;
	pthread_mutex_lock(&exit_mutex);
	pthread_cond_signal(&exit_cond);
	pthread_mutex_unlock(&exit_mutex);
}

int open_backend_file(char *fname)
{
	int fd = -1;
	char buf[FILENAME_MAX];

	strcpy(buf, file_backend_prefix);
	strcat(buf, fname);
	fd = open(buf, O_RDWR);
	if (fd < 0) {
		fprintf(stderr,"Cannot open %s, error %d\n", buf, errno);
	}
	return fd;
}

int unmigrate_backend_file(int dst_fd, int src_fd)
{
	struct stat stbuf;

	if (fstat(src_fd, &stbuf) < 0) {
		fprintf(stderr, "Cannot stat source fd, error %d\n", errno);
		return errno;
	}
	if (sendfile(src_fd, dst_fd, 0, stbuf.st_size) < 0) {
		fprintf(stderr, "sendfile failed, error %d\n", errno);
		return errno;
	}
	return 0;
}

void close_backend_file(int fd)
{
	close(fd);
}

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

	event->state = MIGRATE_INIT;
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

	while (!stopped) {
		int rlen, ret;
		struct unmigrate_event *event;

		event = malloc(sizeof(struct unmigrate_event));
		if (!event) {
			fprintf(stderr, "cannot allocate event, error %d\n",
				errno);
			break;
		}
		memset(event, 0, sizeof(struct unmigrate_event));
		INIT_LIST_HEAD(&event->next);
		event->fanotify_fd = fanotify_fd;

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
		i++;
	}
	return NULL;
}

int main(int argc, char **argv)
{
	int i;
	char init_dir[PATH_MAX];
	int fanotify_fd;

	while ((i = getopt(argc, argv, "b:d:p:")) != -1) {
		switch (i) {
		case 'b':
			if (strcmp(optarg, "file")) {
				fprintf(stderr, "Invalid backend '%s'\n",
					optarg);
				return EINVAL;
			}
			break;
		case 'p':
			strncpy(file_backend_prefix, optarg, FILENAME_MAX);
			break;
		case 'd':
			realpath(optarg, init_dir);
			break;
		default:
			fprintf(stderr, "usage: %s [-d <dir>]\n", argv[0]);
			return EINVAL;
		}
	}
	if (optind < argc) {
		fprintf(stderr, "usage: %s [-d <dir>]\n", argv[0]);
		return EINVAL;
	}
	if ('\0' == file_backend_prefix[0]) {
		fprintf(stderr, "No file backend prefix given\n");
		return EINVAL;
	}
	if ('\0' == init_dir[0]) {
		strcpy(init_dir, "/");
	}
	if (!strcmp(init_dir, "..")) {
		if (chdir(init_dir) < 0) {
			fprintf(stderr,
				"Failed to change to parent directory: %d\n",
				errno);
			return errno;
		}
		sprintf(init_dir, ".");
	}

	if (!strcmp(init_dir, ".") && !getcwd(init_dir, PATH_MAX)) {
		fprintf(stderr, "Failed to get current working directory\n");
		return errno;
	}

	signal_set(SIGINT, sigend);
	signal_set(SIGTERM, sigend);

	fanotify_fd = fanotify_init(FAN_CLASS_PRE_CONTENT, O_RDWR);
	if (fanotify_fd < 0) {
		fprintf(stderr, "cannot start fanotify: error %d\n",
			errno);
		return errno;
	}

	if (fanotify_mark(fanotify_fd, FAN_MARK_ADD,
			  FAN_ACCESS_PERM|FAN_EVENT_ON_CHILD, AT_FDCWD,
			  init_dir) < 0) {
		fprintf(stderr, "cannot set fanotify mark: error %d\n",
			errno);
		return errno;
	}

	watch_fanotify(&fanotify_fd);
#if 0
	pthread_cond_wait(&exit_cond, &exit_mutex);
#endif
	return 0;
}
