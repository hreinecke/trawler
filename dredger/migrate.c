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

#define LOG_AREA "migrate"

int migrate_file(struct backend *be, int fanotify_fd, int src_fd,
		 char *filename)
{
	int migrate_fd;
	int ret;

	migrate_fd = open_backend(be, filename);
	if (migrate_fd < 0) {
		if (errno == EEXIST) {
			info("file '%s' already migrated", filename);
		} else {
			err("failed to open backend file %s, error %d",
			    filename, errno);
		}
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
	return ret;
}
