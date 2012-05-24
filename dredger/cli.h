#ifndef _CLI_H
#define _CLI_H

struct cli_monitor;

enum cli_commands {
    CLI_NONE,
    CLI_SHUTDOWN,
    CLI_MIGRATE,
    CLI_CHECK,
    CLI_MONITOR,
    CLI_NOFILE,
};

pthread_t start_cli(struct backend *be, int fanotify_fd);
void stop_cli(pthread_t cli_thr);
int cli_command(enum cli_commands cli_cmd, char *filename);

#endif /* _CLI_H */
