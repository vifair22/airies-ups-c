#include "config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static void trim(char *s)
{
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
    char *start = s;
    while (*start && isspace((unsigned char)*start)) start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
}

static void set_defaults(config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->ups_device, "/dev/ttyUSB0", CFG_MAX_STR - 1);
    cfg->ups_baud = 9600;
    cfg->ups_slave_id = 1;
    cfg->shutdown_timeout = 180;
}

/* Parse comma-separated host list into cfg->unraid_hosts[] */
static void parse_hosts(config_t *cfg, const char *value)
{
    cfg->unraid_host_count = 0;
    char buf[1024];
    strncpy(buf, value, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *tok = strtok(buf, ",");
    while (tok && cfg->unraid_host_count < CFG_MAX_HOSTS) {
        while (*tok && isspace((unsigned char)*tok)) tok++;
        char *end = tok + strlen(tok) - 1;
        while (end > tok && isspace((unsigned char)*end)) *end-- = '\0';
        if (*tok) {
            strncpy(cfg->unraid_hosts[cfg->unraid_host_count], tok, CFG_MAX_STR - 1);
            cfg->unraid_host_count++;
        }
        tok = strtok(NULL, ",");
    }
}

int config_load(config_t *cfg, const char *path)
{
    set_defaults(cfg);

    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "config: cannot open %s: ", path);
        perror("");
        return -1;
    }

    char line[1024];
    char section[64] = "";

    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (!*line || line[0] == '#' || line[0] == ';')
            continue;

        /* Section header */
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (end) {
                *end = '\0';
                strncpy(section, line + 1, sizeof(section) - 1);
                section[sizeof(section) - 1] = '\0';
            }
            continue;
        }

        /* Key = value */
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trim(key);
        trim(val);

        if (strcmp(section, "ups") == 0) {
            if (strcmp(key, "device") == 0)
                strncpy(cfg->ups_device, val, CFG_MAX_STR - 1);
            else if (strcmp(key, "baud") == 0)
                cfg->ups_baud = atoi(val);
            else if (strcmp(key, "slave_id") == 0)
                cfg->ups_slave_id = atoi(val);
        } else if (strcmp(section, "pushover") == 0) {
            if (strcmp(key, "token") == 0)
                strncpy(cfg->pushover_token, val, CFG_MAX_STR - 1);
            else if (strcmp(key, "user") == 0)
                strncpy(cfg->pushover_user, val, CFG_MAX_STR - 1);
        } else if (strcmp(section, "shutdown") == 0) {
            if (strcmp(key, "unraid_hosts") == 0)
                parse_hosts(cfg, val);
            else if (strcmp(key, "unraid_user") == 0)
                strncpy(cfg->unraid_user, val, CFG_MAX_STR - 1);
            else if (strcmp(key, "unraid_pass") == 0)
                strncpy(cfg->unraid_pass, val, CFG_MAX_STR - 1);
            else if (strcmp(key, "timeout") == 0)
                cfg->shutdown_timeout = atoi(val);
        }
    }

    fclose(f);
    return 0;
}
