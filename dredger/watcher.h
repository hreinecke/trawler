#ifndef _WATCHER_H
#define _WATCHER_H

pthread_t start_watcher(struct backend *be, int fanotify_fd);
void stop_watcher(pthread_t thr);
int check_watcher(char *pathname);

#endif /* _WATCHER_H */

