/*
 * mksparse
 *
 * Create sparse file with the same length and timestamp as the original file
 *
 * Copyright (c) 2012 Hannes Reinecke <hare@suse.de>
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <stdarg.h>
#include <utime.h>
#include <fcntl.h>

#include "logging.h"

#define LOG_AREA "mksparse"

int log_priority = LOG_ERR;
int use_syslog;
FILE *logfd;

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

int main(int argc, char **argv)
{
	int i, fd, fe_offset = 0;
	char fe_prefix[FILENAME_MAX];
	char be_prefix[FILENAME_MAX];
	char *be_file;
	char fe_file[FILENAME_MAX];
	struct stat stbuf;
	struct timeval tv[2];

	logfd = stdout;

	memset(fe_prefix, 0x0, FILENAME_MAX);
	while ((i = getopt(argc, argv, "d:p:")) != -1) {
		switch (i) {
		case 'd':
			log_priority = strtoul(optarg, NULL, 10);
			if (log_priority > LOG_DEBUG) {
				err("Invalid logging priority %d (max %d)",
				    log_priority, LOG_DEBUG);
				goto usage;
			}
			break;
		case 'p':
			if (!realpath(optarg, fe_prefix)) {
				err("Cannot resolve prefix '%s'", optarg);
				return errno;
			}
			break;
		default:
			goto usage;
		}
	}
	if (strlen(fe_prefix))
		fe_offset = strlen(fe_prefix) - 1;
	if (fe_prefix[fe_offset] != '/')
		strcat(fe_prefix, "/");
	if (stat(fe_prefix, &stbuf) < 0) {
		err("Cannot stat() prefix '%s': %d", fe_prefix, errno);
		return errno;
	}
	if (!S_ISDIR(stbuf.st_mode)) {
		err("Prefix '%s' is not a directory", fe_prefix);
		goto usage;
	}
	if (!getcwd(be_prefix, FILENAME_MAX)) {
		err("Cannot resolve working directory: %d", errno);
		return errno;
	}

	if (optind + 1 < argc) {
	usage:
		err("usage: %s [-d <priority>] [-p <dir>]", argv[0]);
		return EINVAL;
	}
	be_file = argv[optind];
	if (strstr(be_file, "..")) {
		err("File '%s' contains backlinks", be_file);
		return EINVAL;
	}
	if (be_file[0] == '/') {
		err("File '%s' is not a relative path", be_file);
		return EINVAL;
	}
	if (be_file[0] == '.') {
		if (be_file[1] == '\0' ||
		    (be_file[1] == '.' && be_file[2] == '\0')) {
			info("Skipping '%s'", be_file);
			return 0;
		}
		if (be_file[1] == '/')
			be_file += 2;
	}
	if (stat(be_file, &stbuf) < 0) {
		err("Cannot stat() file '%s': %d", be_file, errno);
		return errno;
	}
	strcpy(fe_file, fe_prefix);
	strcat(fe_file, be_file);
	tv[0].tv_sec = difftime(stbuf.st_atime, 0);
	tv[0].tv_usec = 0;
	tv[1].tv_sec = difftime(stbuf.st_mtime, 0);
	tv[1].tv_usec = 0;

	if (S_ISREG(stbuf.st_mode)) {
		fd = open(fe_file, O_RDWR|O_CREAT, S_IRWXU);
		if (fd < 0) {
			err("Cannot create file '%s': %d", fe_file, errno);
			return errno;
		}
		/* Create sparse file */
		if (ftruncate(fd, 0) < 0) {
			err("ftruncate failed, error %d", errno);
			return errno;
		}
		if (lseek(fd, stbuf.st_size - 1, SEEK_SET) ==
		    stbuf.st_size -1 ) {
			if (write(fd, "\0", 1) < 1) {
				err("Cannot create sparse file, error %d",
				    errno);
				return errno;
			}
		} else {
			err("Cannot seek to end of sparse file "
			    "(size %d), error %d",
			    stbuf.st_size, errno);
			return errno;
		}
		close(fd);
	} else if (S_ISDIR(stbuf.st_mode)) {
		if (mkdir(fe_file, stbuf.st_mode) < 0) {
			err("Cannot make directory '%s': %d", fe_file, errno);
			return errno;
		}
	} else if (S_ISLNK(stbuf.st_mode)) {
		char buf[FILENAME_MAX];
		int offset = 0;

		if (readlink(be_file, buf, FILENAME_MAX) < 0) {
			err("Cannot resolve link '%s': %d", be_file, errno);
			return errno;
		}
		if (strncmp(buf, fe_prefix, strlen(fe_prefix))) {
			info("Link target '%s' not monitored", buf);
		} else {
			offset = strlen(fe_prefix);
		}
		if (symlink(fe_file, buf + offset) < 0) {
			err("Cannot create link '%s': %d", fe_file, errno);
		}
		info("Symlink '%s' target '%s'", fe_file, buf + offset);
	} else {
		if (mknod(fe_file, stbuf.st_mode, stbuf.st_rdev) < 0) {
			err("cannot make device special file '%s': %d",
			    fe_file, errno);
			return errno;
		}
	}
	/* Set correct user id */
	chown(fe_file, stbuf.st_uid, stbuf.st_gid);
	/* Set correct access rights */
	chmod(fe_file, stbuf.st_mode);
	/* Update timestamp */
	if (utimes(fe_file, tv) < 0) {
		err("cannot update timestamp: %d", errno);
	}
	return 0;
}
