#ifndef _MIGRATE_H
#define _MIGRATE_H

int migrate_file(struct backend *be, int src_fd, char *filename);
int monitor_file(int fanotify_fd, char *filename);

#endif /* _MIGRATE_H */
