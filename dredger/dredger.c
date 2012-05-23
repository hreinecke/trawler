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
#include <sys/syslog.h>
#include <stdarg.h>
#include "fanotify.h"
#include "fanotify-init-syscall.h"
#include "logging.h"
#include "backend.h"
#include "watcher.h"
#include "cli.h"

#define LOG_AREA "watcher"

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

struct backend *backend_list[] = {
	&backend_file,
	NULL
};

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

struct backend *select_backend(const char *name) {
	int i = 0;

	while (backend_list[i]) {
		if (!strcmp(backend_list[i]->name, name))
			return backend_list[i];
		i++;
	}
	return NULL;
}

int main(int argc, char **argv)
{
	int i;
	int fanotify_fd, ret;
	struct backend *be;

	logfd = stdout;

	while ((i = getopt(argc, argv, "b:c:d:m:o:s")) != -1) {
		switch (i) {
		case 'b':
			be = select_backend(optarg);
			if (!be) {
				err("Invalid backend '%s'\n", optarg);
				return EINVAL;
			}
			break;
		case 'c':
			return cli_command(CLI_CHECK, optarg);
			break;
		case 'd':
			log_priority = strtoul(optarg, NULL, 10);
			if (log_priority > LOG_DEBUG) {
				err("Invalid logging priority %d (max %d)",
				    log_priority, LOG_DEBUG);
				exit(1);
			}
			break;
		case 'm':
			ret = cli_command(CLI_CHECK, optarg);
			if (ret)
				return ret;
			return cli_command(CLI_MIGRATE, optarg);
			break;
		case 'o':
			if (be->parse_options(be, optarg) < 0) {
				err("Invalid backend option '%s'", optarg);
				return EINVAL;
			}
			break;
		case 's':
			return cli_command(CLI_SHUTDOWN, NULL);
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

	signal_set(SIGINT, sigend);
	signal_set(SIGTERM, sigend);

	fanotify_fd = fanotify_init(FAN_CLASS_PRE_CONTENT, O_RDWR);
	if (fanotify_fd < 0) {
		fprintf(stderr, "cannot start fanotify, error %d\n",
			errno);
		return errno;
	}

	daemon_thr = pthread_self();

	watcher_thr = start_watcher(be, fanotify_fd);
	if (!watcher_thr)
		return errno;

	cli_thr = start_cli(be, fanotify_fd);
	if (!cli_thr) {
		stop_watcher(watcher_thr);
		return ENOMEM;
	}

	pthread_cond_wait(&exit_cond, &exit_mutex);

	stop_cli(cli_thr);
	stop_watcher(watcher_thr);

	return 0;
}
