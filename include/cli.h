#ifndef _CLI_H
#define _CLI_H

enum cli_commands {
    CLI_NONE,
    CLI_SHUTDOWN,
    CLI_MIGRATE,
    CLI_CHECK,
    CLI_MONITOR,
    CLI_SETUP,
    CLI_NOFILE,
};

int cli_command(enum cli_commands cli_cmd, char *filename);

#endif /* _CLI_H */
