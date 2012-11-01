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

#include "list.h"
#include "logging.h"

#define LOG_AREA "sparse-file"

int make_sparse_file(char *be_prefix, char *be_file, char *fe_prefix)
{
	char fe_file[FILENAME_MAX];
	struct stat stbuf;
	struct timeval tv[2];

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
		int fe_fd;

		fe_fd = open(fe_file, O_RDWR|O_CREAT, S_IRWXU);
		if (fe_fd < 0) {
			err("Cannot create file '%s': %d", fe_file, errno);
			return errno;
		}
		/* Create sparse file */
		if (ftruncate(fe_fd, 0) < 0) {
			err("ftruncate failed, error %d", errno);
			return errno;
		}
		if (lseek(fe_fd, stbuf.st_size - 1, SEEK_SET) ==
		    stbuf.st_size -1 ) {
			if (write(fe_fd, "\0", 1) < 1) {
				err("Cannot create sparse file, error %d",
				    errno);
				close(fe_fd);
				return errno;
			}
		} else {
			err("Cannot seek to end of sparse file "
			    "(size %d), error %d",
			    stbuf.st_size, errno);
			close(fe_fd);
			return errno;
		}
		close(fe_fd);
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
