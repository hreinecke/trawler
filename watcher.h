#ifndef _WATCHER_H
#define _WATCHER_H

int insert_inotify(char *dirname);
int remove_inotify(char *dirname);
int start_watcher(void);
int stop_watcher(void);

#endif /* _WATCHER_H */
