/*
 * backend-file.c
 *
 * File backend for dredger.
 * Copyright (c) 2012 Hannes Reinecke <hare@suse.de>
 */
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sendfile.h>
#include <sys/time.h>
#include <sys/mount.h>
#include <linux/falloc.h>
#include <time.h>
#include <fcntl.h>

#include "logging.h"
#include "list.h"
#include "backend.h"
#include "dredger.h"

#define LOG_AREA "backend-file"

struct backend_file {
	struct backend common;
	size_t thresh;
	char prefix[FILENAME_MAX];
	char filename[FILENAME_MAX];
	int fd;
};

#define to_backend_file(b) container_of(b, struct backend_file, common)

static int get_fname(int fd, char *fname)
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

static int create_leading_directories(char *pathname, mode_t mode)
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

struct backend *new_backend_file(void)
{
	struct backend_file *be;

	be = malloc(sizeof(struct backend_file));
	if (!be)
		return NULL;

	memset(be, 0x0, sizeof(struct backend_file));
	return &be->common;
}

int parse_backend_file_options(struct backend *be, char *args)
{
	struct backend_file *be_file = to_backend_file(be);
	char *value;

	if (strncmp(args, "prefix", 6)) {
		err("Invalid option string '%s'", args);
		return EINVAL;
	}
	value = strchr(args, '=');
	if (!value) {
		err("Invalid option string '%s'", args);
		return EINVAL;
	}
	*value = '\0';
	value++;
	strcpy(be_file->prefix, value);
	return 0;
}

int open_backend_file(struct backend *be, char *fname)
{
	struct backend_file *be_file = to_backend_file(be);
	char buf[FILENAME_MAX];
	int ret = 0;

	strcpy(buf, be_file->prefix);
	strcat(buf, fname);

	if (create_leading_directories(buf, S_IRWXU) < 0)
		return -1;
	be_file->fd = open(buf, O_RDWR|O_CREAT, S_IRWXU);
	if (be_file->fd < 0) {
		err("Cannot open %s, error %d", buf, errno);
		ret = errno;
	} else {
		strcpy(be_file->filename, fname);
		info("Opened backend file '%s'", buf);
	}
	return ret;
}

int check_backend_file(struct backend *be, char *fname)
{
	struct backend_file *be_file = to_backend_file(be);
	char buf[FILENAME_MAX];
	struct stat fe_st, be_st;
	struct tm dtm;

	buf[0] = '\0';
	if (strlen(frontend_prefix))
		strcat(buf, frontend_prefix);
	strcat(buf, fname);
	if (stat(buf, &fe_st) < 0) {
		err("Frontend file '%s' not accessible, error %d",
		    fname, errno);
		return errno;
	}
	if (gmtime_r(&fe_st.st_atime, &dtm)) {
		info("Frontend file '%s', size %d, tstamp "
		     "%04d%02d%02d-%02d%02d%02d", fname, fe_st.st_size,
		     dtm.tm_year + 1900, dtm.tm_mon, dtm.tm_mday,
		     dtm.tm_hour, dtm.tm_min, dtm.tm_sec);
	}
	buf[0] = '\0';
	if (strlen(be_file->prefix))
		strcat(buf, be_file->prefix);
	strcat(buf, fname);
	if (stat(buf, &be_st) < 0)
		return errno;
	if (gmtime_r(&be_st.st_atime, &dtm)) {
		info("Backend file '%s', size %d, tstamp "
		     "%04d%02d%02d-%02d%02d%02d", buf, be_st.st_size,
		     dtm.tm_year + 1900, dtm.tm_mon, dtm.tm_mday,
		     dtm.tm_hour, dtm.tm_min, dtm.tm_sec);
	}
	if (be_st.st_size != fe_st.st_size) {
		info("Backend file '%s' has different size than source file",
		     fname);
		return ESTALE;
	}
	if (difftime(be_st.st_atime, fe_st.st_atime) < 0) {
		info("Backend file '%s' older than source file",
		     fname);
		return ESTALE;
	}
	return 0;
}

/*
 * Migrate frontend file @fd to backend
 */
int migrate_backend_file(struct backend *be, int fe_fd)
{
	struct backend_file *be_file = to_backend_file(be);
	struct stat fe_st, be_st;
	char fe_fname[FILENAME_MAX];
	struct timeval tv[2];
	ssize_t bytes, len;

	if (fstat(be_file->fd, &be_st) < 0) {
		err("Cannot stat backend fd, error %d", errno);
		return errno;
	}
	if (fstat(fe_fd, &fe_st) < 0) {
		err("Cannot stat frontend fd, error %d", errno);
		return errno;
	}
	if (fe_st.st_size != be_st.st_size) {
		info("Updating file size from %ld bytes to %ld bytes",
		     be_st.st_size, fe_st.st_size);
		if (ftruncate(be_file->fd, fe_st.st_size) < 0) {
			err("ftruncate failed, error %d", errno);
			return errno;
		}
	}
	if (be_st.st_dev != fe_st.st_dev) {
		/* Bind mount, just unmount it */
		len = get_fname(fe_fd, fe_fname);
		if (len < 0 || !strlen(fe_fname)) {
			err("cannot resolve frontend filename, error %d",
			    errno);
			return errno;
		}
		if (umount(fe_fname) < 0) {
			err("umount failed, error %d", errno);
			return errno;
		}
		return 0;
	}
	bytes = sendfile(be_file->fd, fe_fd, 0, fe_st.st_size);
	if ( bytes < 0) {
		err("sendfile failed, error %d", errno);
		return errno;
	} else if (bytes < fe_st.st_size) {
		err("sendfile copied only %ld of %ld bytes",
		    bytes, fe_st.st_size);
		return EFBIG;
	}
	if (fchmod(be_file->fd, fe_st.st_mode) < 0) {
		err("cannot set file permissions, error %d", errno);
	}
	if (fchown(be_file->fd, fe_st.st_uid, fe_st.st_gid) < 0) {
		err("cannot update file owner, error %d", errno);
	}
	if (fallocate(fe_fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
		      0, fe_st.st_size) < 0) {
		if (errno != EOPNOTSUPP) {
			err("fallocate failed, error %d", errno);
			return errno;
		}
		/* Fall back to sparse files */
		if (ftruncate(fe_fd, 0) < 0) {
			err("ftruncate failed, error %d", errno);
			return errno;
		}
		/*
		 * Any error from here can be ignored, as we have
		 * successfull migrated the file.
		 */
		if (lseek(fe_fd, fe_st.st_size - 1, SEEK_SET) == 0) {
			if (write(fe_fd, "\0", 1) < 1)
				err("Cannot create sparse file, error %d",
				    errno);
		} else {
			err("Cannot seek to end of sparse file, error %d",
			    errno);
		}
	}
	/* Update timestamp on backend file */
	tv[0].tv_sec = difftime(fe_st.st_atime, 0);
	tv[0].tv_usec = 0;
	tv[1].tv_sec = difftime(fe_st.st_mtime, 0);
	tv[1].tv_usec = 0;
	if (futimes(be_file->fd, tv) < 0) {
		err("cannot update file timestamps, error %d", errno);
	}
	return 0;
}

int unmigrate_backend_file(struct backend *be, int fe_fd)
{
	struct backend_file *be_file = to_backend_file(be);
	struct stat fe_st, be_st;
	struct timeval tv[2];
	ssize_t bytes, len;
	char fe_fname[FILENAME_MAX];
	char be_fname[FILENAME_MAX];

	if (fstat(fe_fd, &fe_st) < 0) {
		err("Cannot stat frontend fd, error %d", errno);
		return errno;
	}
	if (fstat(be_file->fd, &be_st) < 0) {
		err("Cannot stat backend fd, error %d", errno);
		return errno;
	}
	if (be_st.st_size != fe_st.st_size) {
		info("Updating file size from %ld bytes to %ld bytes",
		     fe_st.st_size, be_st.st_size);
		if (posix_fallocate(fe_fd, 0, be_st.st_size) < 0) {
			err("fallocate failed, error %d", errno);
			return errno;
		}
	}
	if (be_st.st_size < be_file->thresh) {
		bytes = sendfile(be_file->fd, fe_fd, 0, be_st.st_size);
		if (bytes < 0) {
			err("sendfile failed, error %d", errno);
			return errno;
		} else if (bytes < be_st.st_size) {
			info("sendfile copied only %ld of %ld bytes",
			    bytes, be_st.st_size);
			goto try_mount;
		}
	} else {
		goto try_mount;
	}
	tv[0].tv_sec = difftime(be_st.st_atime, 0);
	tv[0].tv_usec = 0;
	tv[1].tv_sec = difftime(be_st.st_mtime, 0);
	tv[1].tv_usec = 0;
	if (futimes(fe_fd, tv) < 0) {
		err("cannot update file timestamps, error %d", errno);
	}
	return 0;
try_mount:
	len = get_fname(be_file->fd, be_fname);
	if (len < 0 || !strlen(be_fname)) {
		err("cannot resolve backend filename, error %d", errno);
		return errno;
	}
	len = get_fname(fe_fd, fe_fname);
	if (len < 0 || !strlen(fe_fname)) {
		err("cannot resolve source filename, error %d", errno);
		return errno;
	}
	if (mount(fe_fname, be_fname, NULL, MS_BIND, NULL) < 0) {
		err("bind mount failed, error %d", errno);
		return errno;
	}
	return 0;
}

void close_backend_file(struct backend *be)
{
	struct backend_file *be_file = to_backend_file(be);
#if 0
	char buf[FILENAME_MAX];
#endif

	close(be_file->fd);
#if 0
	gen_backend_filename(be, fname, buf);
	unlink(buf);
#endif
}

struct backend_template backend_file = {
	.name = "file",
	.new = new_backend_file,
	.parse_options = parse_backend_file_options,
	.open = open_backend_file,
	.check = check_backend_file,
	.migrate = migrate_backend_file,
	.unmigrate = unmigrate_backend_file,
	.close = close_backend_file,
};

