#ifndef _MIGRATE_H
#define _MIGRATE_H

int migrate_file(struct backend *be, int src_fd, char *filename);
int unmigrate_file(struct backend *be, int fe_fd, char *filename);
int monitor_file(int fanotify_fd, char *filename);
int unmonitor_file(int fanotify_fd, char *filename);

#endif /* _MIGRATE_H */
