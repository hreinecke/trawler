/*
 * logging.c
 *
 * Common logging functions
 * Copyright (c) 2012 Hannes Reinecke <hare@suse.de>
 */
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

#include "logging.h"

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

