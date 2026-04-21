#ifndef CLI_COMMANDS_H
#define CLI_COMMANDS_H

/* CLI subcommand handlers.
 * Each takes the daemon socket path, full argc/argv, and returns exit code. */

int cmd_status(const char *sock, int argc, char **argv);
int cmd_events(const char *sock, int argc, char **argv);
int cmd_telemetry(const char *sock, int argc, char **argv);
int cmd_cmd(const char *sock, int argc, char **argv);

#endif
