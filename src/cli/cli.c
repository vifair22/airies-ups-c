#include "cli/cli.h"

#include <stdio.h>
#include <string.h>

/* --- Option parsing --- */

const char *opt_get(int argc, char **argv, int start, const char *flag)
{
    for (int i = start; i < argc - 1; i++)
        if (strcmp(argv[i], flag) == 0) return argv[i + 1];
    return NULL;
}

int opt_has(int argc, char **argv, int start, const char *flag)
{
    for (int i = start; i < argc; i++)
        if (strcmp(argv[i], flag) == 0) return 1;
    return 0;
}

int is_flag_token(const char *s)
{
    return s && s[0] == '-' && s[1] == '-';
}

int has_help_flag(int argc, char **argv, int start)
{
    for (int i = start; i < argc; i++)
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
            return 1;
    return 0;
}

static int flag_takes_value(const char *flag, const flag_spec_t *specs,
                            size_t n_specs)
{
    for (size_t i = 0; i < n_specs; i++)
        if (strcmp(flag, specs[i].name) == 0) return specs[i].takes_value;
    return -1;
}

const char *find_subcmd(int argc, char **argv, int start,
                        const flag_spec_t *specs, size_t n_specs)
{
    for (int i = start; i < argc; i++) {
        if (is_flag_token(argv[i])) {
            if (flag_takes_value(argv[i], specs, n_specs) == 1 && i + 1 < argc)
                i++;
            continue;
        }
        return argv[i];
    }
    return NULL;
}

static int is_known_positional(const char *tok, const char *const *positionals,
                               size_t n_positional)
{
    for (size_t i = 0; i < n_positional; i++)
        if (strcmp(tok, positionals[i]) == 0) return 1;
    return 0;
}

int validate_options(int argc, char **argv, int start,
                     const flag_spec_t *specs, size_t n_specs,
                     const char *const *positionals, size_t n_positional)
{
    for (int i = start; i < argc; i++) {
        const char *tok = argv[i];

        if (strcmp(tok, "--help") == 0 || strcmp(tok, "-h") == 0)
            continue;

        if (is_flag_token(tok)) {
            int takes = flag_takes_value(tok, specs, n_specs);
            if (takes < 0) {
                fprintf(stderr, "error: unknown option '%s'\n", tok);
                help_current();
                return 1;
            }
            if (takes && (i + 1 >= argc || is_flag_token(argv[i + 1]))) {
                fprintf(stderr, "error: option '%s' requires a value\n", tok);
                help_current();
                return 1;
            }
            if (takes) i++;
            continue;
        }

        if (positionals && !is_known_positional(tok, positionals, n_positional)) {
            fprintf(stderr, "error: unexpected argument '%s'\n", tok);
            help_current();
            return 1;
        }
    }
    return 0;
}

/* --- Contextual help --- */

static char g_topic[64] = {0};

void set_topic(const char *t)
{
    if (!t) { g_topic[0] = '\0'; return; }
    size_t n = strlen(t);
    if (n >= sizeof(g_topic)) n = sizeof(g_topic) - 1;
    /* nosemgrep: flawfinder.memcpy-1.CopyMemory-1.bcopy-1 -- n is clamped to sizeof(g_topic)-1 above; explicit NUL termination follows */
    memcpy(g_topic, t, n);
    g_topic[n] = '\0';
}

const char *current_topic(void)
{
    return g_topic[0] ? g_topic : NULL;
}

/* --- Help content --- */

typedef struct {
    const char *topic;
    const char *body;
} help_entry_t;

static const help_entry_t HELP[] = {
    { NULL,
      "Usage: airies-ups [--socket <path>] <command> [args]\n"
      "\n"
      "Commands:\n"
      "  status                       Live UPS status\n"
      "  events                       Recent event journal\n"
      "  cmd <action>                 UPS commands\n"
      "\n"
      "Options:\n"
      "  --socket <path>              Unix socket (default: /tmp/airies-ups.sock)\n"
      "  --version, -V                Show version\n"
      "  --help, -h                   Show this help\n"
    },
    { "status",
      "Usage: airies-ups status\n"
      "\n"
      "Show live UPS status including battery, input, output, and driver info.\n"
    },
    { "events",
      "Usage: airies-ups events\n"
      "\n"
      "Show recent entries from the event journal.\n"
    },
    { "cmd",
      "Usage: airies-ups cmd <action> [options]\n"
      "\n"
      "Available actions are determined by the connected UPS driver.\n"
      "Run 'airies-ups cmd' without arguments to see the list.\n"
      "\n"
      "Special actions:\n"
      "  shutdown [--dry-run]         Orchestrated shutdown workflow\n"
      "\n"
      "Toggle commands (e.g., bypass) require 'on' or 'off':\n"
      "  airies-ups cmd bypass on\n"
    },
};

void help_all(void)
{
    fputs(HELP[0].body, stderr);
}

void help_topic(const char *t)
{
    if (!t || !*t) { help_all(); return; }
    size_t n = sizeof(HELP) / sizeof(HELP[0]);
    for (size_t i = 1; i < n; i++) {
        if (strcmp(HELP[i].topic, t) == 0) {
            fputs(HELP[i].body, stderr);
            return;
        }
    }
    help_all();
}

void help_current(void)
{
    help_topic(current_topic());
}
