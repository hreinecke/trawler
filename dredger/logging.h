#ifndef _LOGGING_H
#define _LOGGING_H

#include <syslog.h>

void log_fn(int priority, const char *format, ...);

#define dbg(fmt, args...) log_fn(LOG_DEBUG, fmt "\n", ##args)
#define info(fmt, args...) log_fn(LOG_INFO, fmt "\n", ##args)
#define warn(fmt, args...) log_fn(LOG_WARNING, fmt "\n", ##args)
#define err(fmt, args...) log_fn(LOG_ERR, fmt "\n", ##args)

#endif /* _LOGGING_H */
