#ifndef _LOGGING_H
#define _LOGGING_H

#include <stdio.h>
#include <syslog.h>

void log_fn(int priority, const char *format, ...);

#define dbg(fmt, args...) log_fn(LOG_DEBUG, LOG_AREA ": " fmt "\n", ##args)
#define info(fmt, args...) log_fn(LOG_INFO, LOG_AREA ": " fmt "\n", ##args)
#define warn(fmt, args...) log_fn(LOG_WARNING, LOG_AREA ": " fmt "\n", ##args)
#define err(fmt, args...) log_fn(LOG_ERR, LOG_AREA ": " fmt "\n", ##args)

extern int log_priority;
extern int use_syslog;
extern FILE *logfd;

#endif /* _LOGGING_H */
