#ifndef _CLI_SERVER_H
#define _CLI_SERVER_H

struct cli_monitor;

pthread_t start_cli(struct backend *be, int fanotify_fd);
void stop_cli(pthread_t cli_thr);

#endif /* _CLI_SERVER_H */
