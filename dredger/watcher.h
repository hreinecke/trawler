#ifndef _WATCHER_H
#define _WATCHER_H

pthread_t start_watcher(void);
void stop_watcher(pthread_t thr);

#endif /* _WATCHER_H */

