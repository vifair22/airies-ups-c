#include "cli/cli.h"
#include "cli/commands.h"

#include <stdio.h>
#include <string.h>

#define DEFAULT_SOCKET "/tmp/airies-ups.sock"

/* --- Dispatch table --- */

typedef struct {
    const char *name;
    int (*handler)(const char *sock, int argc, char **argv);
} cmd_entry_t;

static const cmd_entry_t COMMANDS[] = {
    { "status",    cmd_status },
    { "events",    cmd_events },
    { "cmd",       cmd_cmd },
};

static const cmd_entry_t *find_command(const char *name)
{
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++)
        if (strcmp(COMMANDS[i].name, name) == 0) return &COMMANDS[i];
    return NULL;
}

/* --- Main --- */

int main(int argc, char *argv[])
{
    const char *sock = DEFAULT_SOCKET;
    int argi = 1;

    /* Global flags */
    while (argi < argc && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "--socket") == 0 && argi + 1 < argc) {
            sock = argv[++argi];
            argi++;
        } else if (strcmp(argv[argi], "--help") == 0 || strcmp(argv[argi], "-h") == 0) {
            help_all();
            return 0;
        } else if (strcmp(argv[argi], "--version") == 0 || strcmp(argv[argi], "-V") == 0) {
#ifdef VERSION_STRING
            printf("airies-ups %s\n", VERSION_STRING);
#else
            printf("airies-ups (unknown version)\n");
#endif
            return 0;
        } else {
            fprintf(stderr, "error: unknown option '%s'\n", argv[argi]);
            help_all();
            return 1;
        }
    }

    if (argi >= argc) {
        help_all();
        return 1;
    }

    const char *cmd = argv[argi];
    const cmd_entry_t *entry = find_command(cmd);
    if (!entry) {
        fprintf(stderr, "error: unknown command '%s'\n", cmd);
        help_all();
        return 1;
    }

    set_topic(cmd);
    return entry->handler(sock, argc, argv);
}
