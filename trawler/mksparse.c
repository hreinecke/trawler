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

int make_sparse_file(char *be_prefix, char *be_file, char *fe_prefix);

int log_priority = LOG_ERR;
int use_syslog;
FILE *logfd;

int main(int argc, char **argv)
{
	int i, ret, fe_offset = 0;
	char fe_prefix[FILENAME_MAX];
	char be_prefix[FILENAME_MAX];
	struct stat stbuf;

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
	ret = make_sparse_file(be_prefix, argv[optind], fe_prefix);
	return ret;
}
