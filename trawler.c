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
#include "list.h"

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

static int stopped;
static int inotify_q_size = 32;

int trawl_dir(char *dirname, int inotify_fd)
{
	int num_files = 0;
	char fullpath[PATH_MAX];
	struct stat dirst;
	struct tm dtm;
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
	if (!gmtime_r(&dtime, &dtm)) {
		fprintf(stderr, "%s: Cannot convert time, error %d\n",
			dirname, errno);
		return 0;
	}

	if (!S_ISDIR(dirst.st_mode)) {
		struct event_file *ev_file;
		struct event_entry *tmp_ev, *d_ev = NULL, *last_ev = NULL;

		ev_file = malloc(sizeof(struct event_file));
		if (!ev_file) {
			fprintf(stderr, "%s: Cannot allocate memory, error %d\n",
				dirname, errno);
			return 0;
		}
		strcpy(ev_file->ef_path, dirname);

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
			INIT_LIST_HEAD(&d_ev->ee_next);
			INIT_LIST_HEAD(&d_ev->ee_entries);
			d_ev->ee_time = dtime;
			if (last_ev)
				list_add(&d_ev->ee_next, &last_ev->ee_next);
			else
				list_add(&d_ev->ee_next, &event_list);
		}
		list_add(&ev_file->ef_next, &d_ev->ee_entries);
		return 1;
	}
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
			num_files += trawl_dir(fullpath, inotify_fd);
	}
	closedir(dirfd);
	return num_files;
}

#define EVENT_SIZE (sizeof(struct inotify_event))
#define BUF_LEN (1024 * (EVENT_SIZE + 16))

void * watch_dir(void * arg)
{
	int inotify_fd = *(int *)arg;
	fd_set rfd;
	struct timeval tmo;
	char buf[BUF_LEN];

	FD_ZERO(&rfd);
	FD_SET(inotify_fd, &rfd);

	while (!stopped) {
		int rlen, ret, i = 0;

		tmo.tv_sec = 5;
		tmo.tv_usec = 0;
		ret = select(inotify_fd + 1, &rfd, NULL, NULL, &tmo);
		if (ret < 0) {
			if (ret == EINTR)
				continue;
			fprintf(stderr,"select returned %d\n", errno);
			break;
		}
		if (ret == 0) {
			fprintf(stderr, "select timeout\n");
			continue;
		}
		if (!FD_ISSET(inotify_fd, &rfd)) {
			fprintf(stderr, "select returned for invalid fd\n");
			continue;
		}

		rlen = read(inotify_fd, buf, BUF_LEN);
		while (i < rlen) {
			struct inotify_event *in_ev;
			const char *type;

			in_ev = (struct inotify_event *)&(buf[i]);

			if (!in_ev->len) {
				i += EVENT_SIZE + in_ev->len;
				continue;
			}
			if (in_ev->mask & IN_ISDIR) {
				type = "Directory";
			} else {
				type = "File";
			}
			if (in_ev->mask & IN_IGNORED) {
				printf("inotify event %d removed\n",
				       in_ev->wd);
			} else if (in_ev->mask & IN_Q_OVERFLOW) {
				printf("inotify event %d: queue overflow\n",
				       in_ev->wd);
			} else {
				printf("event %d: %s %x\n",
				       in_ev->wd, in_ev->name, in_ev->mask);
				if (in_ev->mask & IN_CREATE)
					printf("\tcreated %s %s\n",
					       type, in_ev->name);
				if (in_ev->mask & IN_DELETE)
					printf("\tdeleted %s %s\n",
					       type, in_ev->name);
				if (in_ev->mask & IN_MODIFY)
					printf("\tmodified %s %s\n",
					       type, in_ev->name);
				if (in_ev->mask & IN_OPEN)
					printf("\topened %s %s\n",
					       type, in_ev->name);
				if (in_ev->mask & IN_CLOSE)
					printf("\tclosed %s %s\n",
					       type, in_ev->name);
			}
			i += EVENT_SIZE + in_ev->len;
		}
	}

	return NULL;
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
#if 0
		printf("%04d%02d%02d-%02d%02d%02d:\n",
		       dtm.tm_year + 1900, dtm.tm_mon, dtm.tm_mday,
		       dtm.tm_hour, dtm.tm_min, dtm.tm_sec);
#endif
		num_files = 0;
		list_for_each_entry(tmp_ef, &tmp_ee->ee_entries, ef_next) {
			num_files++;
#if 0
			printf("\t%s\n", tmp_ef->ef_path);
#endif
		}
		printf("%04d%02d%02d-%02d%02d%02d: %d entries\n",
		       dtm.tm_year + 1900, dtm.tm_mon, dtm.tm_mday,
		       dtm.tm_hour, dtm.tm_min, dtm.tm_sec, num_files);
	}
}

int main(int argc, char **argv)
{
	int i, num_files, inotify_fd, inotify_wd;
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
	inotify_fd = inotify_init();
	if (inotify_fd < 0) {
		fprintf(stderr, "Failed to initialize inotify, error %d\n",
			errno);
		return errno;
	}

	printf("Starting at '%s'\n", init_dir);
	num_files = trawl_dir(init_dir, inotify_fd);
	printf("Scanned %d files\n", num_files);

	printf("Starting inotify\n");
	inotify_wd = inotify_add_watch(inotify_fd, init_dir, IN_ALL_EVENTS);
	if (inotify_wd < 0) {
		fprintf(stderr, "%s: inotify_add_watch failed with %d\n",
			init_dir, errno);
		return 0;
	}
	printf("%s: added inotify watch\n", init_dir);
	watch_dir(&inotify_fd);
	inotify_rm_watch(inotify_fd, inotify_wd);

	return 0;
}
