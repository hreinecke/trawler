#ifndef _MIGRATE_H
#define _MIGRATE_H

int migrate_file(struct backend *be, int fanotify_fd, int fd, char *filename);

#endif /* _MIGRATE_H */
