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
#include <search.h>
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

struct event_watch {
	int ew_wd;
	char ew_path[PATH_MAX];
};

LIST_HEAD(event_list);
void *watch_tree = NULL;

static int stopped;

int insert_event(char *dirname, time_t dtime)
{
	struct tm dtm;
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

	if (!gmtime_r(&dtime, &dtm)) {
		fprintf(stderr, "%s: Cannot convert time, error %d\n",
			dirname, errno);
		return -errno;
	}
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

int insert_inotify(char *dirname, int inotify_fd)
{
	struct event_watch *ew;
	void *val;

	ew = malloc(sizeof(struct event_watch));
	if (!ew) {
		fprintf(stderr, "%s: cannot allocate watch entry\n", dirname);
		return -ENOMEM;
	}
	ew->ew_wd = inotify_add_watch(inotify_fd, dirname, IN_ALL_EVENTS);
	if (ew->ew_wd < 0) {
		fprintf(stderr, "%s: inotify_add_watch failed with %d\n",
			dirname, errno);
		free(ew);
		return -errno;
	}
	strcpy(ew->ew_path, dirname);
	val = tsearch((void *)ew, &watch_tree, compare_wd);
	if (!val) {
		fprintf(stderr, "%s: Failed to insert watch entry\n", dirname);
		inotify_rm_watch(inotify_fd, ew->ew_wd);
		free(ew);
		return -ENOMEM;
	} else if ((*(struct event_watch **) val) != ew) {
		fprintf(stderr, "%s: watch %d already present\n", dirname,
			ew->ew_wd);
		free(ew);
		return -EEXIST;
	}
	printf("%s: added inotify watch %d\n", ew->ew_path, ew->ew_wd);
	return 0;
}

int remove_inotify(char *dirname, int inotify_fd)
{
	void *val;
	struct event_watch ew, *found_ew;

	strcpy(ew.ew_path, dirname);
	val = tfind((void *)&ew, &watch_tree, compare_path);
	if (!val) {
		fprintf(stderr, "%s: watch entry not found in tree", dirname);
		return -EINVAL;
	}
	found_ew = *(struct event_watch **)val;
	val = tdelete((void *)found_ew, &watch_tree, compare_wd);
	if (!val) {
		fprintf(stderr, "%s: failed to remove in watch entry",
			dirname);
		return -EINVAL;
	}
	inotify_rm_watch(inotify_fd, found_ew->ew_wd);
	printf("%s: removed inotify watch %d\n",
	       found_ew->ew_path, found_ew->ew_wd);
	free(found_ew);

	return 0;
}

int trawl_dir(char *dirname, int inotify_fd)
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
	if (insert_inotify(dirname, inotify_fd) < 0)
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
			void *val;
			struct event_watch ew, *found_ew;
			const char *op;
			char path[PATH_MAX];

			in_ev = (struct inotify_event *)&(buf[i]);

			if (!in_ev->len)
				goto next;

			ew.ew_wd = in_ev->wd;
			val = tfind((void *)&ew, &watch_tree,
					 compare_wd);
			if (!val) {
				fprintf(stderr, "inotify event %d not found "
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
				printf("inotify event %d removed\n",
				       in_ev->wd);
				goto next;
			}
			if (in_ev->mask & IN_Q_OVERFLOW) {
				printf("inotify event %d: queue overflow\n",
				       in_ev->wd);
				goto next;
			}
			printf("event %d: %x\n",
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
			printf("\t%s %s %s\n", op, type, path);
			if (in_ev->mask & IN_ISDIR) {
				if ((in_ev->mask & IN_DELETE) ||
				    (in_ev->mask & IN_MOVED_FROM))
					remove_inotify(path, inotify_fd);
				if ((in_ev->mask & IN_CREATE) ||
				    (in_ev->mask & IN_MOVED_TO))
					insert_inotify(path, inotify_fd);
			}
		next:
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
	int i, num_files, inotify_fd;
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

	list_events();

	printf("Starting inotify\n");
	watch_dir(&inotify_fd);

	return 0;
}
