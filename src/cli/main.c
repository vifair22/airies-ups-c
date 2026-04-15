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

    /* Build HTTP request */
    char req[4096];
    int n;
    if (body) {
        n = snprintf(req, sizeof(req),
            "%s %s HTTP/1.0\r\n"
            "Host: localhost\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %zu\r\n"
            "\r\n"
            "%s",
            method, path, strlen(body), body);
    } else {
        n = snprintf(req, sizeof(req),
            "%s %s HTTP/1.0\r\n"
            "Host: localhost\r\n"
            "\r\n",
            method, path);
    }

    if (write(fd, req, (size_t)n) != n) {
        close(fd);
        return -1;
    }

    /* Read response */
    size_t total = 0;
    while (total < resp_sz - 1) {
        ssize_t r = read(fd, response + total, resp_sz - 1 - total);
        if (r <= 0) break;
        total += (size_t)r;
    }
    response[total] = '\0';
    close(fd);

    /* Find body (after \r\n\r\n) */
    char *body_start = strstr(response, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        memmove(response, body_start, strlen(body_start) + 1);
    }

    return 0;
}

/* --- Pretty print JSON --- */

static void print_json(const char *json)
{
    cJSON *parsed = cJSON_Parse(json);
    if (parsed) {
        char *pretty = cJSON_Print(parsed);
        if (pretty) {
            printf("%s\n", pretty);
            free(pretty);
        }
        cJSON_Delete(parsed);
    } else {
        printf("%s\n", json);
    }
}

/* --- Commands --- */

static int cmd_status(const char *sock)
{
    char resp[BUF_SIZE];
    if (http_request(sock, "GET", "/api/status", NULL, resp, sizeof(resp)) != 0)
        return 1;
    print_json(resp);
    return 0;
}

static int cmd_events(const char *sock)
{
    char resp[BUF_SIZE];
    if (http_request(sock, "GET", "/api/events", NULL, resp, sizeof(resp)) != 0)
        return 1;
    print_json(resp);
    return 0;
}

static int cmd_telemetry(const char *sock)
{
    char resp[BUF_SIZE];
    if (http_request(sock, "GET", "/api/telemetry", NULL, resp, sizeof(resp)) != 0)
        return 1;
    print_json(resp);
    return 0;
}

static int cmd_action(const char *sock, const char *action,
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

/* --- Usage --- */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [--socket <path>] <command> [args]\n"
        "\n"
        "Commands:\n"
        "  status                       Live UPS status\n"
        "  events                       Recent event journal\n"
        "  telemetry                    Recent telemetry data\n"
        "  cmd shutdown [--dry-run]     Trigger shutdown workflow\n"
        "  cmd battery-test             Start battery self-test\n"
        "  cmd runtime-cal              Start runtime calibration\n"
        "  cmd bypass on|off            Toggle bypass mode\n"
        "  cmd freq <setting>           Set frequency tolerance\n"
        "  cmd mute                     Mute alarms\n"
        "  cmd unmute                   Unmute alarms\n"
        "  cmd beep                     Beeper test\n"
        "  cmd clear-faults             Clear fault register\n"
        "\n"
        "Options:\n"
        "  --socket <path>              Unix socket (default: %s)\n",
        prog, DEFAULT_SOCKET);
}

int main(int argc, char *argv[])
{
    const char *sock = DEFAULT_SOCKET;
    int argi = 1;

    /* Parse global options */
    while (argi < argc && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "--socket") == 0 && argi + 1 < argc) {
            sock = argv[++argi];
            argi++;
        } else if (strcmp(argv[argi], "--help") == 0 || strcmp(argv[argi], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[argi]);
            usage(argv[0]);
            return 1;
        }
    }

    if (argi >= argc) {
        usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[argi++];

    if (strcmp(cmd, "status") == 0)
        return cmd_status(sock);

    if (strcmp(cmd, "events") == 0)
        return cmd_events(sock);

    if (strcmp(cmd, "telemetry") == 0)
        return cmd_telemetry(sock);

    if (strcmp(cmd, "cmd") == 0) {
        if (argi >= argc) {
            fprintf(stderr, "error: 'cmd' requires an action\n");
            return 1;
        }
        const char *action = argv[argi++];

        if (strcmp(action, "shutdown") == 0) {
            int dry_run = (argi < argc && strcmp(argv[argi], "--dry-run") == 0);
            if (dry_run) {
                /* Send with dry_run flag */
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
            return cmd_action(sock, "shutdown", NULL, NULL);
        }

        if (strcmp(action, "battery-test") == 0)
            return cmd_action(sock, "battery_test", NULL, NULL);
        if (strcmp(action, "runtime-cal") == 0)
            return cmd_action(sock, "runtime_cal", NULL, NULL);
        if (strcmp(action, "clear-faults") == 0)
            return cmd_action(sock, "clear_faults", NULL, NULL);
        if (strcmp(action, "mute") == 0)
            return cmd_action(sock, "mute", NULL, NULL);
        if (strcmp(action, "unmute") == 0)
            return cmd_action(sock, "unmute", NULL, NULL);
        if (strcmp(action, "beep") == 0)
            return cmd_action(sock, "beep", NULL, NULL);

        if (strcmp(action, "bypass") == 0 && argi < argc) {
            const char *dir = argv[argi];
            if (strcmp(dir, "on") == 0)
                return cmd_action(sock, "bypass_on", NULL, NULL);
            if (strcmp(dir, "off") == 0)
                return cmd_action(sock, "bypass_off", NULL, NULL);
            fprintf(stderr, "error: bypass expects 'on' or 'off'\n");
            return 1;
        }

        if (strcmp(action, "freq") == 0 && argi < argc)
            return cmd_action(sock, "freq", "setting", argv[argi]);

        fprintf(stderr, "error: unknown command action '%s'\n", action);
        return 1;
    }

    fprintf(stderr, "error: unknown command '%s'\n", cmd);
    usage(argv[0]);
    return 1;
}
