/*
 * backend-file.c
 *
 * File backend for dredger.
 * Copyright (c) 2012 Hannes Reinecke <hare@suse.de>
 */
#include <stdio.h>
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sendfile.h>
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>

#include "logging.h"
#include "backend.h"

#define LOG_AREA "backend-file"

struct backend_file_options {
	char prefix[FILENAME_MAX];
} file_options;

int create_leading_directories(char *pathname, mode_t mode)
{
	char dirname[FILENAME_MAX], *p;
	struct stat st;
	int ret;

	info("Lookup path component '%s'", pathname);
	strcpy(dirname, pathname);
	p = strrchr(dirname, '/');
	if (!p)
		return 0;
	*p = '\0';
	if (stat(dirname, &st) == 0) {
		if (!S_ISDIR(st.st_mode)) {
			err("Path component '%s' is not a directory",
			    dirname);
			return ENOTDIR;
		}
		return 0;
	}
	if (errno != ENOENT) {
		err("Path lookup for '%s' failed, error %d",
		    pathname, errno);
		return errno;
	}
	ret = create_leading_directories(dirname, mode);
	if (ret)
		return ret;
	info("Create path component '%s'", dirname);
	return mkdir(dirname, mode);
}

int parse_backend_options(struct backend *be, char *optionstr)
{
	struct backend_file_options *opts = be->options;
	char *value;

	if (strncmp(optionstr, "prefix", 6)) {
		err("Invalid option string '%s'", optionstr);
		return EINVAL;
	}
	value = strchr(optionstr, '=');
	if (!value) {
		err("Invalid option string '%s'", optionstr);
		return EINVAL;
	}
	*value = '\0';
	value++;
	strcpy(opts->prefix, value);
	return 0;
}

int open_backend_file(struct backend *be, char *fname)
{
	struct backend_file_options *opts = be->options;
	int fd = -1;
	char buf[FILENAME_MAX];

	strcpy(buf, opts->prefix);
	strcat(buf, fname);
	if (create_leading_directories(buf, S_IRWXU) < 0)
		return -1;
	fd = open(buf, O_RDWR|O_CREAT, S_IRWXU);
	if (fd < 0) {
		err("Cannot open %s, error %d", buf, errno);
	}
	info("Opened backend file '%s'", buf);
	return fd;
}

int check_backend_file(struct backend *be, char *fname)
{
	struct backend_file_options *opts = be->options;
	char buf[FILENAME_MAX];
	struct stat src_st, backend_st;
	struct tm dtm;

	if (stat(fname, &src_st) < 0) {
		err("Source file '%s' not accessible, error %d", fname, errno);
		return errno;
	}
	if (gmtime_r(&src_st.st_atime, &dtm)) {
		info("Source file '%s', size %d, tstamp "
		     "%04d%02d%02d-%02d%02d%02d", fname, src_st.st_size,
		     dtm.tm_year + 1900, dtm.tm_mon, dtm.tm_mday,
		     dtm.tm_hour, dtm.tm_min, dtm.tm_sec);
	}
	strcpy(buf, opts->prefix);
	strcat(buf, fname);
	if (stat(buf, &backend_st) < 0)
		return errno;
	if (gmtime_r(&backend_st.st_atime, &dtm)) {
		info("Backend file '%s', size %d, tstamp "
		     "%04d%02d%02d-%02d%02d%02d", buf, backend_st.st_size,
		     dtm.tm_year + 1900, dtm.tm_mon, dtm.tm_mday,
		     dtm.tm_hour, dtm.tm_min, dtm.tm_sec);
	}
	if (backend_st.st_size != src_st.st_size) {
		info("Backend file '%s' has different size than source file",
		     fname);
		return ESTALE;
	}
	if (difftime(backend_st.st_atime, src_st.st_atime) < 0) {
		info("Backend file '%s' older than source file",
		     fname);
		return ESTALE;
	}
	return 0;
}

int migrate_backend_file(struct backend *be, int be_fd, int orig_fd)
{
	struct stat orig_st, be_st;
	struct timeval tv[2];

	if (fstat(be_fd, &be_st) < 0) {
		err("Cannot stat backend fd, error %d", errno);
		return errno;
	}
	if (fstat(orig_fd, &orig_st) < 0) {
		err("Cannot stat source fd, error %d", errno);
		return errno;
	}
	if (orig_st.st_size != be_st.st_size) {
		info("Updating file size from %ld bytes to %ld bytes",
		       be_st.st_size, orig_st.st_size);
		if (ftruncate(be_fd, orig_st.st_size) < 0) {
			err("ftruncate failed, error %d", errno);
			return errno;
		}
	}
	if (sendfile(be_fd, orig_fd, 0, orig_st.st_size) < 0) {
		err("sendfile failed, error %d", errno);
		return errno;
	}
	if (fchmod(be_fd, orig_st.st_mode) < 0) {
		err("cannot set file permissions, error %d", errno);
	}
	if (fchown(be_fd, orig_st.st_uid, orig_st.st_gid) < 0) {
		err("cannot update file owner, error %d", errno);
	}
	if (ftruncate(orig_fd, 0) < 0) {
		err("ftruncate failed, error %d", errno);
		return errno;
	}
	/*
	 * Any error from here can be ignored, as we have
	 * successfull migrated the file.
	 */
	if (lseek(orig_fd, orig_st.st_size - 1, SEEK_SET) < 0) {
		err("Cannot seek to end of sparse file, error %d", errno);
		return 0;
	}
	if (write(orig_fd, "\0", 1) < 1)
		err("Cannot create sparse file, error %d", errno);
	tv[0].tv_sec = difftime(orig_st.st_atime, 0);
	tv[0].tv_usec = 0;
	tv[1].tv_sec = difftime(orig_st.st_mtime, 0);
	tv[1].tv_usec = 0;
	if (futimes(be_fd, tv) < 0) {
		err("cannot update file timestamps, error %d", errno);
	}
	return 0;
}

int unmigrate_backend_file(struct backend *be, int be_fd, int orig_fd)
{
	struct stat orig_st, be_st;
	struct timeval tv[2];

	if (fstat(be_fd, &be_st) < 0) {
		err("Cannot stat backend fd, error %d", errno);
		return errno;
	}
	if (fstat(orig_fd, &orig_st) < 0) {
		err("Cannot stat orig fd, error %d", errno);
		return errno;
	}
	if (be_st.st_size != orig_st.st_size) {
		info("Updating file size from %ld bytes to %ld bytes",
		     orig_st.st_size, be_st.st_size);
		if (posix_fallocate(orig_fd, 0, be_st.st_size) < 0) {
			err("fallocate failed, error %d", errno);
			return errno;
		}
	}
	if (sendfile(be_fd, orig_fd, 0, be_st.st_size) < 0) {
		err("sendfile failed, error %d", errno);
		return errno;
	}
	tv[0].tv_sec = difftime(be_st.st_atime, 0);
	tv[0].tv_usec = 0;
	tv[1].tv_sec = difftime(be_st.st_mtime, 0);
	tv[1].tv_usec = 0;
	if (futimes(orig_fd, tv) < 0) {
		err("cannot update file timestamps, error %d", errno);
	}
	return 0;
}

void close_backend_file(struct backend *be, char *fname, int fd)
{
#if 0
	struct backend_file_options *opts = be->options;
	char buf[FILENAME_MAX];
#endif

	close(fd);
#if 0
	strcpy(buf, opts->prefix);
	strcat(buf, fname);
	unlink(buf);
#endif
}

struct backend backend_file = {
	.name = "file",
	.parse_options = parse_backend_options,
	.open = open_backend_file,
	.check = check_backend_file,
	.migrate = migrate_backend_file,
	.unmigrate = unmigrate_backend_file,
	.close = close_backend_file,
	.options = &file_options,
};

