#ifndef IPC_H
#define IPC_H

#include <stddef.h>

#define IPC_MAX_CMD_LEN   64
#define IPC_MAX_RESPONSE  4096
#define IPC_SENTINEL       ".\n"

/* Parsed IPC command */
typedef enum {
    IPC_CMD_STATUS,
    IPC_CMD_BYPASS_ON,
    IPC_CMD_BYPASS_OFF,
    IPC_CMD_MODE_HE,
    IPC_CMD_MODE_ONLINE,
    IPC_CMD_TEST_BATTERY,
    IPC_CMD_CLEAR_FAULTS,
    IPC_CMD_MUTE,
    IPC_CMD_UNMUTE,
    IPC_CMD_BEEP,
    IPC_CMD_UNKNOWN
} ipc_cmd_t;

/* Server-side (monitor) */
int  ipc_server_init(const char *sock_path);
void ipc_server_cleanup(const char *sock_path, int listen_fd);

/* Client-side (one-shot) */
int  ipc_client_connect(const char *sock_path);
int  ipc_client_send(int fd, const char *cmd);
int  ipc_client_recv(int fd, char *buf, size_t bufsz);

/* Command parsing */
ipc_cmd_t ipc_parse_command(const char *line);

/* Response helpers */
void ipc_respond(int client_fd, const char *fmt, ...);
void ipc_respond_end(int client_fd);

/* PID file lock */
int  ipc_lock_acquire(const char *lock_path);
void ipc_lock_release(int lock_fd, const char *lock_path);

#endif
