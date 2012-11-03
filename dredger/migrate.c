/*
 * migrate.c
 *
 * Migrate files to backend.
 * Copyright (c) 2012 Hannes Reinecke <hare@suse.de>
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include "fanotify.h"
#include "fanotify-mark-syscall.h"

#include "logging.h"
#include "backend.h"
#include "migrate.h"

#define LOG_AREA "migrate"

int migrate_file(struct backend *be, int fe_fd, char *filename)
{
	int ret;

	ret = open_backend(be, filename);
	if (ret) {
		if (ret == EEXIST) {
			info("file '%s' already migrated", filename);
		} else {
			err("failed to open backend file %s, error %d",
			    filename, ret);
		}
		return errno;
	}
	if (fe_fd < 0) {
		info("start setup file '%s'", filename);
		ret = setup_backend(be);
	} else {
		info("start migration on file '%s'", filename);
		ret = migrate_backend(be, fe_fd);
	}
	close_backend(be);
	if (ret) {
		err("failed to %s file %s, error %d",
		    fe_fd < 0 ? "setup" : "migrate", filename, ret);
	} else {
		info("finished %s file '%s'",
		     fe_fd < 0 ? "setup" : "migrating", filename);
	}
	return ret;
}

int unmigrate_file(struct backend *be, int fe_fd, char *filename)
{
	int ret;

	ret = open_backend(be, filename);
	if (ret) {
		if (ret == ENOENT) {
			info("backend file %s already un-migrated",
			     filename);
			ret = 0;
		} else {
			err("failed to open backend file %s, error %d",
			    filename, ret);
		}
		return ret;
	}
	info("start un-migration on file '%s'", filename);
	ret = unmigrate_backend(be, fe_fd);
	if (ret < 0) {
		err("failed to unmigrate file %s, error %d",
		    filename, ret);
	} else {
		info("finished un-migration on file '%s'", filename);
	}
	close_backend(be);

	return ret;
}

int monitor_file(int fanotify_fd, char *filename)
{
	int ret;

	info("Set fanotify_mark on '%s'", filename);
	ret = fanotify_mark(fanotify_fd, FAN_MARK_ADD,
			    FAN_ACCESS_PERM|FAN_EVENT_ON_CHILD,
			    AT_FDCWD, filename);
	if (ret < 0) {
		err("failed to add fanotify mark "
		    "to %s, error %d\n", filename, errno);
		ret = -ret;
	}
	return ret;
}

int unmonitor_file(int fanotify_fd, char *filename)
{
	int ret;

	ret = fanotify_mark(fanotify_fd, FAN_MARK_REMOVE,
			    FAN_ACCESS_PERM|FAN_EVENT_ON_CHILD,
			    AT_FDCWD, filename);
	if (ret < 0) {
		err("failed to remove fanotify mark "
		    "from %s, error %d", filename, errno);
	}
	return ret;
}
