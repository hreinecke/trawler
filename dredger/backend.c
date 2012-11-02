/*
 * backend.c
 *
 * Backend wrapper functions
 * Copyright (c) 2012 Hannes Reinecke <hare@suse.de>
 */
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

#include "logging.h"
#include "backend.h"

#define LOG_AREAD "backend"

extern struct backend_template backend_file;

struct backend_template *backend_list[] = {
	&backend_file,
	NULL
};

static struct backend_template *select_backend(const char *name) {
	int i = 0;

	while (backend_list[i]) {
		if (!strcmp(backend_list[i]->name, name))
			return backend_list[i];
		i++;
	}
	return NULL;
}

struct backend *new_backend(const char *name) {
	struct backend *be = NULL;
	struct backend_template *te = select_backend(name);

	if (!te)
		return NULL;

	be = te->new();
	if (!be)
		return NULL;

	memset(be, 0x0, sizeof(struct backend));
	be->template = te;

	return be;
}

int parse_backend_options(struct backend *be, char *optarg) {
	if (!be || !be->template->parse_options)
		return EINVAL;

	return be->template->parse_options(be, optarg);
}

int open_backend(struct backend *be, char *filename) {
	if (!be || !be->template->open)
		return EINVAL;

	return be->template->open(be, filename);
}

int check_backend(struct backend *be, char *filename) {
	if (!be || !be->template->check)
		return EINVAL;

	return be->template->check(be, filename);
}

int setup_backend(struct backend *be) {
	if (!be || !be->template->migrate)
		return EINVAL;

	return be->template->migrate(be, -1);
}

int migrate_backend(struct backend *be, int fe_fd) {
	if (!be || !be->template->migrate)
		return EINVAL;

	return be->template->migrate(be, fe_fd);
}

int unmigrate_backend(struct backend *be, int fe_fd) {
	if (!be || !be->template->unmigrate)
		return EINVAL;

	return be->template->unmigrate(be, fe_fd);
}

void close_backend(struct backend *be) {
	if (!be || !be->template->close)
		return;

	be->template->close(be);
}

