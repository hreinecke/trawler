#ifndef _CLI_H
#define _CLI_H

struct cli_monitor;

pthread_t start_cli(int fanotify_fd);
void stop_cli(pthread_t cli_thr);
int cli_command(char *cmd);

#endif /* _CLI_H */
