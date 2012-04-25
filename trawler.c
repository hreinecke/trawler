/*
 * trawler.c
 *
 * Main routine for trawler.
 * Copyright (c) 2012 Hannes Reinecke <hare@suse.de>
 */
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <time.h>
#include <dirent.h>
#include <limits.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include "list.h"
#include "watcher.h"

struct event_file {
	struct list_head ef_next;
	char ef_path[PATH_MAX];
};

struct event_entry {
	struct list_head ee_next;
	struct list_head ee_entries;
	time_t ee_time;
};

LIST_HEAD(event_list);
pthread_cond_t exit_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t exit_mutex = PTHREAD_MUTEX_INITIALIZER;

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
	pthread_mutex_lock(&exit_mutex);
	pthread_cond_signal(&exit_cond);
	pthread_mutex_unlock(&exit_mutex);
}

int insert_event(char *dirname, time_t dtime)
{
	struct event_file *tmp_ef, *d_ef = NULL, *new_ef;
	struct event_entry *tmp_ev, *d_ev = NULL, *last_ev = NULL;
	char *ptr;

	new_ef = malloc(sizeof(struct event_file));
	if (!new_ef) {
		fprintf(stderr, "%s: Cannot allocate memory, error %d\n",
			dirname, errno);
		return -errno;
	}
	strcpy(new_ef->ef_path, dirname);
	ptr = strrchr(new_ef->ef_path, '/');
	if (ptr)
		*ptr = '\0';

	list_for_each_entry(tmp_ev, &event_list, ee_next) {
		if (difftime(tmp_ev->ee_time, dtime) > 0)
			continue;
		if (tmp_ev->ee_time == dtime) {
			d_ev = tmp_ev;
			break;
		}
		last_ev = tmp_ev;
	}
	if (!d_ev) {
		d_ev = malloc(sizeof(struct event_entry));
		if (!d_ev) {
			fprintf(stderr,"%s: failed to allocate event entry\n",
				dirname);
			return -ENOMEM;
		}
		INIT_LIST_HEAD(&d_ev->ee_next);
		INIT_LIST_HEAD(&d_ev->ee_entries);
		d_ev->ee_time = dtime;
		if (last_ev)
			list_add(&d_ev->ee_next, &last_ev->ee_next);
		else
			list_add(&d_ev->ee_next, &event_list);
	} else {
		list_for_each_entry(tmp_ef, &d_ev->ee_entries, ef_next) {
			if (!strcmp(tmp_ef->ef_path, new_ef->ef_path)) {
				d_ef = tmp_ef;
				break;
			}
		}
		if (d_ef) {
			free(new_ef);
			return 0;
		}
	}
	list_add(&new_ef->ef_next, &d_ev->ee_entries);
	return 0;
}

int trawl_dir(char *dirname)
{
	int num_files = 0;
	char fullpath[PATH_MAX];
	struct stat dirst;
	time_t dtime;
	DIR *dirfd;
	struct dirent *dirent;

	if (stat(dirname, &dirst) < 0) {
		fprintf(stderr, "Cannot open %s: error %d\n",
			dirname, errno);
		return 0;
	}
	if (difftime(dirst.st_atime, dirst.st_mtime) < 0) {
		dtime = dirst.st_atime;
	} else {
		dtime = dirst.st_mtime;
	}

	if (!S_ISDIR(dirst.st_mode)) {
		if (insert_event(dirname, dtime) < 0)
			return 0;
		else
			return 1;
	}
	if (insert_inotify(dirname) < 0)
		return 0;

	dirfd = opendir(dirname);
	if (!dirfd) {
		fprintf(stderr, "Cannot open directory %s: error %d\n",
			dirname, errno);
		return 0;
	}
	while ((dirent = readdir(dirfd))) {
		if (!strcmp(dirent->d_name, "."))
			continue;
		if (!strcmp(dirent->d_name, ".."))
			continue;
		if(snprintf(fullpath, PATH_MAX, "%s/%s",
			    dirname, dirent->d_name) >= PATH_MAX) {
			fprintf(stderr, "%s/%s: pathname overflow\n",
				dirname, dirent->d_name);
		}
		if ((dirent->d_type & DT_REG) || (dirent->d_type & DT_DIR))
			num_files += trawl_dir(fullpath);
	}
	closedir(dirfd);
	return num_files;
}

void list_events(void)
{
	struct event_file *tmp_ef;
	struct event_entry *tmp_ee;
	struct tm dtm;
	int num_files;

	list_for_each_entry(tmp_ee, &event_list, ee_next) {
		if (!gmtime_r(&tmp_ee->ee_time, &dtm)) {
			fprintf(stderr, "%s: Cannot convert time, error %d\n",
				tmp_ef->ef_path, errno);
			continue;
		}
		printf("%04d%02d%02d-%02d%02d%02d:\n",
		       dtm.tm_year + 1900, dtm.tm_mon, dtm.tm_mday,
		       dtm.tm_hour, dtm.tm_min, dtm.tm_sec);
		num_files = 0;
		list_for_each_entry(tmp_ef, &tmp_ee->ee_entries, ef_next) {
			num_files++;
			printf("\t%s\n", tmp_ef->ef_path);
		}
		printf("%04d%02d%02d-%02d%02d%02d: %d entries\n",
		       dtm.tm_year + 1900, dtm.tm_mon, dtm.tm_mday,
		       dtm.tm_hour, dtm.tm_min, dtm.tm_sec, num_files);
	}
}

int main(int argc, char **argv)
{
	int i, num_files;
	char init_dir[PATH_MAX];

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

	start_watcher();

	printf("Starting at '%s'\n", init_dir);
	num_files = trawl_dir(init_dir);
	printf("Scanned %d files\n", num_files);

	list_events();

	pthread_cond_wait(&exit_cond, &exit_mutex);
	stop_watcher();

	return 0;
}
