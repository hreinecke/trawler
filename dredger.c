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
#include "fanotify.h"
#include "fanotify-syscall.h"

pthread_cond_t exit_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t exit_mutex = PTHREAD_MUTEX_INITIALIZER;

int stopped;

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

void * watch_fanotify(void * arg)
{
	int fanotify_fd = *(int *)arg;
	fd_set rfd;
	struct timeval tmo;
	char fname[PATH_MAX];
	int i = 0;

	while (!stopped) {
		struct fanotify_event_metadata event;
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

		rlen = read(fanotify_fd, &event, sizeof(event));
		if (rlen < 0) {
			fprintf(stderr, "error %d on reading fanotify\n",
				errno);
			continue;
		}
		if (get_fname(event.fd, fname) < 0) {
			fprintf(stderr, "cannot retrieve filename\n");
		}
		printf("fanotify event %d: mask 0x%02lX, fd %d (%s), pid %d\n",
		       i, (unsigned long) event.mask, event.fd, fname,
		       event.pid);
		if (event.mask & FAN_ACCESS_PERM) {
			struct fanotify_response resp;

			resp.fd = event.fd;
			resp.response = FAN_ALLOW;
			if (write(fanotify_fd, &resp, sizeof(resp)) < 0) {
				fprintf(stderr, "Failed to write fanotify "
					"response: error %d\n", errno);
			} else {
				printf("Wrote response\n");
			}
		}
		close(event.fd);
		i++;
	}
	return NULL;
}

int main(int argc, char **argv)
{
	int i;
	char init_dir[PATH_MAX];
	int fanotify_fd;

	while ((i = getopt(argc, argv, "d:")) != -1) {
		switch (i) {
		case 'd':
			realpath(optarg, init_dir);
			break;
		default:
			fprintf(stderr, "usage: %s [-d <dir>]\n", argv[0]);
			return 1;
		}
	}
	if (optind < argc) {
		fprintf(stderr, "usage: %s [-d <dir>]\n", argv[0]);
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
