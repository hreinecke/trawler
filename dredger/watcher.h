#ifndef _WATCHER_H
#define _WATCHER_H

pthread_t start_watcher(struct backend *be, int fanotify_fd);
void stop_watcher(pthread_t thr);
int check_watcher(char *pathname);
int migrate_file(struct backend *be, int fanotify_fd, char *filename);

#endif /* _WATCHER_H */

