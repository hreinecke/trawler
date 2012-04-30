/*
 * events.c
 *
 * event handling for trawler.
 * Copyright (c) 2012 Hannes Reinecke <hare@suse.de>
 */
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <time.h>
#include "list.h"
#include "events.h"

struct event_file {
	struct list_head ef_next;
	char ef_path[PATH_MAX];
};

struct event_entry {
	struct list_head ee_next;
	struct list_head ee_entries;
	time_t ee_time;
};

LIST_HEAD(event_list);

int insert_event(char *dirname, time_t dtime)
{
	struct event_file *tmp_ef, *d_ef = NULL, *new_ef;
	struct event_entry *tmp_ev, *d_ev = NULL, *last_ev = NULL;
	char *ptr;

	new_ef = malloc(sizeof(struct event_file));
	if (!new_ef) {
		fprintf(stderr, "%s: Cannot allocate memory, error %d\n",
			dirname, errno);
		return -errno;
	}
	strcpy(new_ef->ef_path, dirname);
	ptr = strrchr(new_ef->ef_path, '/');
	if (ptr)
		*ptr = '\0';

	list_for_each_entry(tmp_ev, &event_list, ee_next) {
		if (difftime(tmp_ev->ee_time, dtime) > 0)
			continue;
		if (tmp_ev->ee_time == dtime) {
			d_ev = tmp_ev;
			break;
		}
		last_ev = tmp_ev;
	}
	if (!d_ev) {
		d_ev = malloc(sizeof(struct event_entry));
		if (!d_ev) {
			fprintf(stderr,"%s: failed to allocate event entry\n",
				dirname);
			return -ENOMEM;
		}
		INIT_LIST_HEAD(&d_ev->ee_next);
		INIT_LIST_HEAD(&d_ev->ee_entries);
		d_ev->ee_time = dtime;
		if (last_ev)
			list_add(&d_ev->ee_next, &last_ev->ee_next);
		else
			list_add(&d_ev->ee_next, &event_list);
	} else {
		list_for_each_entry(tmp_ef, &d_ev->ee_entries, ef_next) {
			if (!strcmp(tmp_ef->ef_path, new_ef->ef_path)) {
				d_ef = tmp_ef;
				break;
			}
		}
		if (d_ef) {
			free(new_ef);
			return 0;
		}
	}
	list_add(&new_ef->ef_next, &d_ev->ee_entries);
	return 0;
}

void list_events(void)
{
	struct event_file *tmp_ef;
	struct event_entry *tmp_ee;
	struct tm dtm;
	int num_files;

	list_for_each_entry(tmp_ee, &event_list, ee_next) {
		if (!gmtime_r(&tmp_ee->ee_time, &dtm)) {
			fprintf(stderr, "%s: Cannot convert time, error %d\n",
				tmp_ef->ef_path, errno);
			continue;
		}
		printf("%04d%02d%02d-%02d%02d%02d:\n",
		       dtm.tm_year + 1900, dtm.tm_mon, dtm.tm_mday,
		       dtm.tm_hour, dtm.tm_min, dtm.tm_sec);
		num_files = 0;
		list_for_each_entry(tmp_ef, &tmp_ee->ee_entries, ef_next) {
			num_files++;
			printf("\t%s\n", tmp_ef->ef_path);
		}
		printf("%04d%02d%02d-%02d%02d%02d: %d entries\n",
		       dtm.tm_year + 1900, dtm.tm_mon, dtm.tm_mday,
		       dtm.tm_hour, dtm.tm_min, dtm.tm_sec, num_files);
	}
}
