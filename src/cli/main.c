#include "cli/cli.h"
#include <cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

#define DEFAULT_SOCKET "/tmp/airies-ups.sock"
#define BUF_SIZE 65536

/* --- HTTP over unix socket --- */

static int sock_connect(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int http_request(const char *socket_path, const char *method,
                        const char *path, const char *body,
                        char *response, size_t resp_sz)
{
    int fd = sock_connect(socket_path);
    if (fd < 0) {
        fprintf(stderr, "error: cannot connect to daemon at %s\n", socket_path);
        fprintf(stderr, "is airies-upsd running?\n");
        return -1;
    }

    char req[4096];
    int n;
    if (body) {
        n = snprintf(req, sizeof(req),
            "%s %s HTTP/1.0\r\n"
            "Host: localhost\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "\r\n%s",
            method, path, strlen(body), body);
    } else {
        n = snprintf(req, sizeof(req),
            "%s %s HTTP/1.0\r\n"
            "Host: localhost\r\n"
            "\r\n",
            method, path);
    }

    if (write(fd, req, (size_t)n) != n) { close(fd); return -1; }

    size_t total = 0;
    while (total < resp_sz - 1) {
        ssize_t r = read(fd, response + total, resp_sz - 1 - total);
        if (r <= 0) break;
        total += (size_t)r;
    }
    response[total] = '\0';
    close(fd);

    char *body_start = strstr(response, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        memmove(response, body_start, strlen(body_start) + 1);
    }
    return 0;
}

static void print_json(const char *json)
{
    cJSON *parsed = cJSON_Parse(json);
    if (parsed) {
        char *pretty = cJSON_Print(parsed);
        if (pretty) { printf("%s\n", pretty); free(pretty); }
        cJSON_Delete(parsed);
    } else {
        printf("%s\n", json);
    }
}

/* --- Command handlers --- */

static int cmd_status(const char *sock, int argc, char **argv)
{
    if (has_help_flag(argc, argv, 2)) { help_current(); return 0; }

    char resp[BUF_SIZE];
    if (http_request(sock, "GET", "/api/status", NULL, resp, sizeof(resp)) != 0)
        return 1;
    print_json(resp);
    return 0;
}

static int cmd_events(const char *sock, int argc, char **argv)
{
    if (has_help_flag(argc, argv, 2)) { help_current(); return 0; }

    char resp[BUF_SIZE];
    if (http_request(sock, "GET", "/api/events", NULL, resp, sizeof(resp)) != 0)
        return 1;
    print_json(resp);
    return 0;
}

static int cmd_telemetry(const char *sock, int argc, char **argv)
{
    if (has_help_flag(argc, argv, 2)) { help_current(); return 0; }

    char resp[BUF_SIZE];
    if (http_request(sock, "GET", "/api/telemetry", NULL, resp, sizeof(resp)) != 0)
        return 1;
    print_json(resp);
    return 0;
}

static int send_action(const char *sock, const char *action,
                       const char *extra_key, const char *extra_val)
{
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "action", action);
    if (extra_key && extra_val)
        cJSON_AddStringToObject(body, extra_key, extra_val);
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);

    char resp[BUF_SIZE];
    int rc = http_request(sock, "POST", "/api/cmd", json, resp, sizeof(resp));
    free(json);
    if (rc != 0) return 1;
    print_json(resp);
    return 0;
}

static int cmd_cmd(const char *sock, int argc, char **argv)
{
    static const flag_spec_t global_flags[] = {
        { "--socket", 1 },
    };

    const char *sub = find_subcmd(argc, argv, 2,
                                  global_flags,
                                  sizeof(global_flags) / sizeof(global_flags[0]));
    if (!sub) {
        if (has_help_flag(argc, argv, 2)) { help_topic("cmd"); return 0; }
        fprintf(stderr, "error: cmd requires an action\n");
        help_topic("cmd");
        return 1;
    }

    /* Refine help topic */
    char topic[64];
    snprintf(topic, sizeof(topic), "cmd %s", sub);
    set_topic(topic);

    if (has_help_flag(argc, argv, 2)) { help_current(); return 0; }

    /* Dispatch actions */
    if (strcmp(sub, "shutdown") == 0) {
        if (opt_has(argc, argv, 3, "--dry-run")) {
            cJSON *body = cJSON_CreateObject();
            cJSON_AddStringToObject(body, "action", "shutdown");
            cJSON_AddBoolToObject(body, "dry_run", 1);
            char *json = cJSON_PrintUnformatted(body);
            cJSON_Delete(body);
            char resp[BUF_SIZE];
            int rc = http_request(sock, "POST", "/api/cmd", json, resp, sizeof(resp));
            free(json);
            if (rc != 0) return 1;
            print_json(resp);
            return 0;
        }
        return send_action(sock, "shutdown", NULL, NULL);
    }

    if (strcmp(sub, "battery-test") == 0) return send_action(sock, "battery_test", NULL, NULL);
    if (strcmp(sub, "runtime-cal") == 0)  return send_action(sock, "runtime_cal", NULL, NULL);
    if (strcmp(sub, "clear-faults") == 0) return send_action(sock, "clear_faults", NULL, NULL);
    if (strcmp(sub, "mute") == 0)         return send_action(sock, "mute", NULL, NULL);
    if (strcmp(sub, "unmute") == 0)       return send_action(sock, "unmute", NULL, NULL);
    if (strcmp(sub, "beep") == 0)         return send_action(sock, "beep", NULL, NULL);

    if (strcmp(sub, "bypass") == 0) {
        const char *dir = find_subcmd(argc, argv, 3, NULL, 0);
        if (!dir || (strcmp(dir, "on") != 0 && strcmp(dir, "off") != 0)) {
            fprintf(stderr, "error: bypass requires 'on' or 'off'\n");
            help_current();
            return 1;
        }
        return send_action(sock, strcmp(dir, "on") == 0 ? "bypass_on" : "bypass_off",
                           NULL, NULL);
    }

    if (strcmp(sub, "freq") == 0) {
        const char *setting = find_subcmd(argc, argv, 3, NULL, 0);
        if (!setting) {
            fprintf(stderr, "error: freq requires a setting name\n");
            help_current();
            return 1;
        }
        return send_action(sock, "freq", "setting", setting);
    }

    fprintf(stderr, "error: unknown action '%s'\n", sub);
    help_topic("cmd");
    return 1;
}

/* --- Dispatch table --- */

typedef struct {
    const char *name;
    int (*handler)(const char *sock, int argc, char **argv);
} cmd_entry_t;

static const cmd_entry_t COMMANDS[] = {
    { "status",    cmd_status },
    { "events",    cmd_events },
    { "telemetry", cmd_telemetry },
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
