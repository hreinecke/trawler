#ifndef _DREDGER_H
#define _DREDGER_H

extern int daemon_stopped;
extern pthread_t daemon_thr;

int open_backend_file(char *fname);
int check_backend_file(char *fname);
int migrate_backend_file(int dst_fd, int src_fd);
int unmigrate_backend_file(int dst_fd, int src_fd);
void close_backend_file(int fd);

#endif /* _DREDGER_H */
