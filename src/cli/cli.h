#ifndef CLI_H
#define CLI_H

#include <stddef.h>

/* --- Option parsing ---
 *
 * Lightweight, zero-allocation option parsing with whitelist validation.
 * Pattern: define flag specs, validate, extract values. */

/* Describes one known CLI flag */
typedef struct {
    const char *name;        /* e.g., "--socket" */
    int         takes_value; /* 1 if flag consumes next argv as its argument */
} flag_spec_t;

/* Return the value following --flag, or NULL if not found. */
const char *opt_get(int argc, char **argv, int start, const char *flag);

/* Return 1 if --flag appears in argv[start..]. */
int opt_has(int argc, char **argv, int start, const char *flag);

/* Return 1 if s starts with "--". */
int is_flag_token(const char *s);

/* Return 1 if --help or -h is present in argv[start..]. */
int has_help_flag(int argc, char **argv, int start);

/* Find the first positional (non-flag) token, skipping known flags + values. */
const char *find_subcmd(int argc, char **argv, int start,
                        const flag_spec_t *specs, size_t n_specs);

/* Validate argv[start..] against whitelist of known flags and positionals.
 * Prints error + contextual help on failure. Returns 0 on success, 1 on error. */
int validate_options(int argc, char **argv, int start,
                     const flag_spec_t *specs, size_t n_specs,
                     const char *const *positionals, size_t n_positional);

/* --- Contextual help --- */

/* Set the active help topic. Pass NULL to clear. */
void set_topic(const char *t);

/* Get the current topic, or NULL. */
const char *current_topic(void);

/* Print the full top-level command index to stderr. */
void help_all(void);

/* Print help for a specific topic. Falls back to help_all() if unknown. */
void help_topic(const char *t);

/* Print help for the current topic (or top-level if none set). */
void help_current(void);

#endif
