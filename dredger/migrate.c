/*
 * migrate.c
 *
 * Migrate files to backend.
 * Copyright (c) 2012 Hannes Reinecke <hare@suse.de>
 */
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include "fanotify.h"
#include "fanotify-mark-syscall.h"

#include "logging.h"
#include "backend.h"
#include "migrate.h"

int migrate_file(struct backend *be, int fanotify_fd, char *filename)
{
	int src_fd, migrate_fd;
	struct flock lock;
	int ret;

	src_fd = open(filename, O_RDWR);
	if (src_fd < 0) {
		err("Cannot open source file '%s', error %d",
		    filename, errno);
		return errno;
	}
	info("Locking file '%s'", filename);
	lock.l_type = F_WRLCK;
	lock.l_start = 0;
	lock.l_whence = SEEK_SET;
	lock.l_len = 0;
	ret = fcntl(src_fd, F_SETLK, &lock);
	if (ret) {
		if (errno == EAGAIN ||
		    errno == EACCES) {
			ret = fcntl(src_fd, F_GETLK, &lock);
			if (ret) {
				err("Could not get lock on source file '%s', "
				    "error %d", filename, errno);
			} else {
				/* Previous migration, wait for completion */
				lock.l_type = F_WRLCK;
				ret = fcntl(src_fd, F_SETLKW, &lock);
				/* close() releases the lock */
				close(src_fd);
				return ret;
			}
		} else {
			err("Cannot lock source file '%s', error %d",
			    filename, errno);
			close(src_fd);
			return errno;
		}
	}
	/* We could place the lock ok */
	migrate_fd = open_backend(be, filename);
	if (migrate_fd < 0) {
		if (errno == EEXIST) {
			info("file '%s' already migrated", filename);
		} else {
			err("failed to open backend file %s, error %d",
			    filename, errno);
		}
		close(src_fd);
		return errno;
	}
	info("start migration on file '%s'", filename);
	ret = migrate_backend(be, migrate_fd, src_fd);
	close_backend(be, filename, migrate_fd);
	if (ret) {
		err("failed to migrate file %s, error %d",
		    filename, ret);
	} else {
		info("finished migration on file '%s'", filename);
		info("Set fanotify_mark on '%s'", filename);
		ret = fanotify_mark(fanotify_fd, FAN_MARK_ADD,
				    FAN_ACCESS_PERM|FAN_EVENT_ON_CHILD,
				    AT_FDCWD, filename);
		if (ret < 0) {
			err("failed to add fanotify mark "
			    "to %s, error %d\n", filename, errno);
			ret = -ret;
		}
	}
	close(src_fd);
	return ret;
}
