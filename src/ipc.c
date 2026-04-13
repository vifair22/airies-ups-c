#include "ipc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/file.h>

/* --- Command table --- */

static const struct {
    const char *text;
    ipc_cmd_t   cmd;
} cmd_table[] = {
    { "STATUS",       IPC_CMD_STATUS },
    { "BYPASS ON",    IPC_CMD_BYPASS_ON },
    { "BYPASS OFF",   IPC_CMD_BYPASS_OFF },
    { "MODE HE",      IPC_CMD_MODE_HE },
    { "MODE ONLINE WEATHER", IPC_CMD_MODE_ONLINE_WEATHER },
    { "MODE ONLINE",  IPC_CMD_MODE_ONLINE },
    { "TEST BATTERY", IPC_CMD_TEST_BATTERY },
    { "CLEAR FAULTS", IPC_CMD_CLEAR_FAULTS },
    { "MUTE",         IPC_CMD_MUTE },
    { "UNMUTE",       IPC_CMD_UNMUTE },
    { "BEEP",         IPC_CMD_BEEP },
};

static int strcasecmp_trimmed(const char *a, const char *b)
{
    while (*a && isspace((unsigned char)*a)) a++;
    size_t len = strlen(a);
    while (len > 0 && isspace((unsigned char)a[len - 1])) len--;

    size_t blen = strlen(b);
    if (len != blen) return 1;
    for (size_t i = 0; i < len; i++) {
        if (toupper((unsigned char)a[i]) != toupper((unsigned char)b[i]))
            return 1;
    }
    return 0;
}

ipc_cmd_t ipc_parse_command(const char *line)
{
    size_t n = sizeof(cmd_table) / sizeof(cmd_table[0]);
    for (size_t i = 0; i < n; i++) {
        if (strcasecmp_trimmed(line, cmd_table[i].text) == 0)
            return cmd_table[i].cmd;
    }
    return IPC_CMD_UNKNOWN;
}

/* --- Server --- */

int ipc_server_init(const char *sock_path)
{
    /* Remove stale socket */
    unlink(sock_path);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    if (listen(fd, 1) < 0) {
        close(fd);
        unlink(sock_path);
        return -1;
    }

    /* Non-blocking so poll() works correctly */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    return fd;
}

void ipc_server_cleanup(const char *sock_path, int listen_fd)
{
    if (listen_fd >= 0)
        close(listen_fd);
    if (sock_path)
        unlink(sock_path);
}

/* --- Client --- */

int ipc_client_connect(const char *sock_path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    /* 5 second send/recv timeout */
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

int ipc_client_send(int fd, const char *cmd)
{
    size_t len = strlen(cmd);
    if (write(fd, cmd, len) != (ssize_t)len) return -1;
    /* Ensure newline terminated */
    if (len == 0 || cmd[len - 1] != '\n') {
        if (write(fd, "\n", 1) != 1) return -1;
    }
    return 0;
}

int ipc_client_recv(int fd, char *buf, size_t bufsz)
{
    size_t total = 0;
    while (total < bufsz - 1) {
        ssize_t n = read(fd, buf + total, bufsz - 1 - total);
        if (n < 0) return -1;
        if (n == 0) break; /* EOF */
        total += (size_t)n;
        buf[total] = '\0';

        /* Check for sentinel: line containing only "." */
        char *sentinel = strstr(buf, "\n.\n");
        if (sentinel) {
            sentinel[1] = '\0'; /* Trim sentinel and everything after */
            return 0;
        }
        /* Also handle sentinel at start of buffer (response is just ".\n") */
        if (total >= 2 && buf[0] == '.' && buf[1] == '\n') {
            buf[0] = '\0';
            return 0;
        }
    }
    return 0;
}

/* --- Response helpers --- */

void ipc_respond(int client_fd, const char *fmt, ...)
{
    char buf[IPC_MAX_RESPONSE];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0)
        write(client_fd, buf, (size_t)n);
}

void ipc_respond_end(int client_fd)
{
    write(client_fd, ".\n", 2);
}

/* --- PID lock --- */

int ipc_lock_acquire(const char *lock_path)
{
    int fd = open(lock_path, O_CREAT | O_WRONLY, 0644);
    if (fd < 0) return -1;

    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        close(fd);
        return -1;
    }

    /* Write our PID */
    if (ftruncate(fd, 0) == 0) {
        char pid_str[32];
        int n = snprintf(pid_str, sizeof(pid_str), "%d\n", (int)getpid());
        if (n > 0)
            write(fd, pid_str, (size_t)n);
    }

    return fd;
}

void ipc_lock_release(int lock_fd, const char *lock_path)
{
    if (lock_fd >= 0)
        close(lock_fd);
    if (lock_path)
        unlink(lock_path);
}
