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
#include <sys/syslog.h>
#include <stdarg.h>
#include "fanotify.h"
#include "fanotify-syscall.h"
#include "list.h"
#include "logging.h"
#include "watcher.h"
#include "cli.h"

pthread_cond_t exit_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t exit_mutex = PTHREAD_MUTEX_INITIALIZER;

pthread_t watcher_thr;
pthread_t cli_thr;
pthread_t daemon_thr;

int daemon_stopped;
char file_backend_prefix[FILENAME_MAX];
int log_priority = LOG_ERR;
int use_syslog;
FILE *logfd;

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
	daemon_stopped = 1;
	pthread_mutex_lock(&exit_mutex);
	pthread_cond_signal(&exit_cond);
	pthread_mutex_unlock(&exit_mutex);
}

void log_fn(int priority, const char *format, ...)
{
	va_list ap;
	time_t curtime;
	struct tm *cur_tm;

	if (log_priority < priority)
		return;

	va_start(ap, format);

	if (use_syslog)
		vsyslog(priority, format, ap);
	else {
		char timestr[32];

		time(&curtime);
		cur_tm = gmtime(&curtime);
		strftime(timestr, 32, "%a %d %T ", cur_tm);
		fprintf(logfd, "%s", timestr);
		vfprintf(logfd, format, ap);
	}
	va_end(ap);
}

int open_backend_file(char *fname)
{
	int fd = -1;
	char buf[FILENAME_MAX];

	strcpy(buf, file_backend_prefix);
	strcat(buf, fname);
	fd = open(buf, O_RDWR|O_CREAT, S_IRWXU);
	if (fd < 0) {
		err("Cannot open %s, error %d", buf, errno);
	}
	return fd;
}

int check_backend_file(char *fname)
{
	char buf[FILENAME_MAX];
	struct stat st;

	strcpy(buf, file_backend_prefix);
	strcat(buf, fname);
	if (stat(buf, &st) < 0)
		return errno;

	return 0;
}

int migrate_backend_file(int dst_fd, int src_fd)
{
	struct stat src_st, dst_st;

	if (fstat(dst_fd, &dst_st) < 0) {
		err("Cannot stat destination fd, error %d", errno);
		return errno;
	}
	if (fstat(src_fd, &src_st) < 0) {
		err("Cannot stat source fd, error %d", errno);
		return errno;
	}
	if (src_st.st_size < dst_st.st_size) {
		info("Updating file size from %ld bytes to %ld bytes",
		       dst_st.st_size, src_st.st_size);
		if (posix_fallocate(dst_fd, 0, src_st.st_size) < 0) {
			err("fallocate failed, error %d", errno);
			return errno;
		}
	}
	if (sendfile(src_fd, dst_fd, 0, src_st.st_size) < 0) {
		err("sendfile failed, error %d", errno);
		return errno;
	}
	return 0;
}

int unmigrate_backend_file(int dst_fd, int src_fd)
{
	struct stat src_st, dst_st;

	if (fstat(dst_fd, &dst_st) < 0) {
		err("Cannot stat destination fd, error %d", errno);
		return errno;
	}
	if (fstat(src_fd, &src_st) < 0) {
		err("Cannot stat source fd, error %d", errno);
		return errno;
	}
	if (dst_st.st_size < src_st.st_size) {
		info("Updating file size from %ld bytes to %ld bytes",
		     dst_st.st_size, src_st.st_size);
		if (posix_fallocate(dst_fd, 0, src_st.st_size) < 0) {
			err("fallocate failed, error %d", errno);
			return errno;
		}
	}
	if (sendfile(src_fd, dst_fd, 0, src_st.st_size) < 0) {
		err("sendfile failed, error %d", errno);
		return errno;
	}
	return 0;
}

void close_backend_file(int fd)
{
	close(fd);
}

int main(int argc, char **argv)
{
	int i;
	int fanotify_fd;

	logfd = stdout;

	while ((i = getopt(argc, argv, "b:c:d:p:")) != -1) {
		switch (i) {
		case 'b':
			if (strcmp(optarg, "file")) {
				fprintf(stderr, "Invalid backend '%s'\n",
					optarg);
				return EINVAL;
			}
			break;
		case 'c':
			return cli_command(optarg);
			break;
		case 'd':
			log_priority = strtoul(optarg, NULL, 10);
			if (log_priority > LOG_DEBUG) {
				err("Invalid logging priority %d (max %d)",
				    log_priority, LOG_DEBUG);
				exit(1);
			}
			break;
		case 'p':
			strncpy(file_backend_prefix, optarg, FILENAME_MAX);
			break;
		default:
			fprintf(stderr, "usage: %s [-d <dir>]\n", argv[0]);
			return EINVAL;
		}
	}
	if (optind < argc) {
		fprintf(stderr, "usage: %s [-b file] [-p <dir>]\n", argv[0]);
		return EINVAL;
	}
	if ('\0' == file_backend_prefix[0]) {
		fprintf(stderr, "No file backend prefix given\n");
		return EINVAL;
	}

	signal_set(SIGINT, sigend);
	signal_set(SIGTERM, sigend);

	fanotify_fd = fanotify_init(FAN_CLASS_PRE_CONTENT, O_RDWR);
	if (fanotify_fd < 0) {
		fprintf(stderr, "cannot start fanotify, error %d\n",
			errno);
		return errno;
	}

	daemon_thr = pthread_self();

	watcher_thr = start_watcher(fanotify_fd);
	if (!watcher_thr)
		return errno;

	cli_thr = start_cli(fanotify_fd);
	if (!cli_thr) {
		stop_watcher(watcher_thr);
		return ENOMEM;
	}

	pthread_cond_wait(&exit_cond, &exit_mutex);

	stop_cli(cli_thr);
	stop_watcher(watcher_thr);

	return 0;
}
