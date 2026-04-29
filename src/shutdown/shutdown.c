#include "shutdown/shutdown.h"
#include <cutils/db.h>
#include <cutils/log.h>
#include <cutils/error.h>
#include <cutils/mem.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/select.h>
#include <time.h>

struct shutdown_mgr {
    cutils_db_t        *db;
    ups_t              *ups;
    cutils_config_t    *config;
    shutdown_progress_fn progress_fn;
    void               *progress_ud;

    /* Trigger state */
    time_t              trigger_first_met;  /* when condition first became true (0=not met) */
    int                 triggered;          /* 1 = workflow already fired, don't re-trigger */
};

/* Internal helpers, non-static so the unit tests can exercise them via
 * extern prototypes. Not part of any public header. */
char *write_key_to_tmpfs(const char *key_material);
int fire_target_action(const char *method, const char *host,
                       const char *username, const char *credential,
                       const char *command);

/* Flush stdio before forking a child so the child can't pick up half-
 * written buffered bytes from the parent (which journald renders as
 * `_LINE_BREAK=pid-change` blobs and drops on the floor). Wraps system(3)
 * so every shell-out path through this module gets the same treatment. */
static int run_shell(const char *cmd)
{
    fflush(stdout);
    fflush(stderr);
    return system(cmd);
}

/* --- Result accumulator ---
 *
 * Phase routines append rows here as they run; the API serializes the
 * final array back to the caller so the UI can show a per-step pass/
 * fail breakdown instead of a single "shutdown initiated" toast. */

static void result_append(shutdown_result_t *res, const char *phase,
                          const char *target, int ok, const char *err)
{
    if (!res) return;
    size_t cap = res->n_steps + 1;
    shutdown_step_result_t *grow = realloc(res->steps, cap * sizeof(*grow));
    if (!grow) return;          /* out of memory: drop the row, keep going */
    res->steps = grow;

    shutdown_step_result_t *row = &res->steps[res->n_steps];
    memset(row, 0, sizeof(*row));
    snprintf(row->phase,  sizeof(row->phase),  "%s", phase  ? phase  : "");
    snprintf(row->target, sizeof(row->target), "%s", target ? target : "");
    row->ok = ok;
    if (ok == 1)
        snprintf(row->error, sizeof(row->error), "%s", err ? err : "");

    res->n_steps++;
    if (ok == 1) res->n_failed++;
}

void shutdown_result_free(shutdown_result_t *res)
{
    if (!res) return;
    free(res->steps);
    free(res);
}

/* --- Progress reporting --- */

static void report(shutdown_mgr_t *mgr, const char *group,
                   const char *target, const char *status)
{
    log_info("[%s/%s] %s", group, target, status);
    if (mgr->progress_fn)
        mgr->progress_fn(group, target, status, mgr->progress_ud);
}

/* --- Confirmation methods --- */

static int is_host_online(const char *host)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ping -c 1 -W 1 %s >/dev/null 2>&1", host);
    return run_shell(cmd) == 0;
}

/* Wait for host to stop responding to ping. Returns 0 if confirmed down,
 * -1 if timed out while still online. */
static int confirm_ping(const char *host, int timeout_sec)
{
    time_t start = time(NULL);
    while (is_host_online(host)) {
        if (time(NULL) - start >= timeout_sec)
            return -1;
        sleep(5);
    }
    return 0;
}

/* Wait for a TCP port to become unreachable (connect refused or timeout).
 * Returns 0 if confirmed down, -1 if still reachable at timeout. */
static int confirm_tcp_port(const char *host, int port, int timeout_sec)
{
    time_t start = time(NULL);
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    for (;;) {
        struct addrinfo hints = { .ai_socktype = SOCK_STREAM };
        struct addrinfo *res = NULL;
        int gai = getaddrinfo(host, port_str, &hints, &res);
        if (gai != 0) return 0; /* can't resolve = down */

        int fd = socket(res->ai_family, SOCK_STREAM, 0);
        if (fd < 0) { freeaddrinfo(res); return 0; }

        /* Non-blocking connect with 2s timeout */
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        int cr = connect(fd, res->ai_addr, res->ai_addrlen);
        if (cr == 0) {
            /* Connected immediately — still up */
            close(fd);
            freeaddrinfo(res);
        } else if (errno == EINPROGRESS) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            struct timeval tv = { .tv_sec = 2 };
            int sel = select(fd + 1, NULL, &wfds, NULL, &tv);
            if (sel > 0) {
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
                close(fd);
                freeaddrinfo(res);
                if (err != 0) return 0; /* connect failed = down */
                /* err == 0 means connected = still up */
            } else {
                close(fd);
                freeaddrinfo(res);
                return 0; /* select timeout = can't connect = down */
            }
        } else {
            /* Connect failed immediately = down */
            close(fd);
            freeaddrinfo(res);
            return 0;
        }

        if (time(NULL) - start >= timeout_sec)
            return -1;
        sleep(5);
    }
}

/* Wait for a command to return 0 (meaning target is confirmed down).
 * Returns 0 if confirmed, -1 if timed out. */
static int confirm_command(const char *cmd, int timeout_sec)
{
    time_t start = time(NULL);
    for (;;) {
        if (run_shell(cmd) == 0)
            return 0;
        if (time(NULL) - start >= timeout_sec)
            return -1;
        sleep(5);
    }
}

/* Dispatch confirmation based on method string. Returns 0 if confirmed
 * down (or method is "none"), -1 on timeout. */
static int confirm_target_down(const char *method, const char *host,
                               int port, const char *cmd, int timeout_sec)
{
    if (!method || strcmp(method, "none") == 0)
        return 0;
    if (strcmp(method, "ping") == 0)
        return confirm_ping(host, timeout_sec);
    if (strcmp(method, "tcp_port") == 0)
        return confirm_tcp_port(host, port, timeout_sec);
    if (strcmp(method, "command") == 0 && cmd)
        return confirm_command(cmd, timeout_sec);
    return 0;
}

/* --- Target execution --- */

/* Write SSH private key material to a tmpfs file for `ssh -i`. Uses
 * /dev/shm (tmpfs on every Linux kernel and inside Docker by default) so
 * the key never touches non-volatile storage. Caller must unlink() and
 * free() the returned path. mkstemp creates the file with mode 0600.
 *
 * Non-static so the unit test in tests/test_shutdown.c can exercise it
 * directly via an `extern` prototype — no public header entry. */
char *write_key_to_tmpfs(const char *key_material)
{
    char template[] = "/dev/shm/airies-ups-key.XXXXXX";
    int fd = mkstemp(template);
    if (fd < 0) {
        set_error(CUTILS_ERR_IO, "mkstemp: %s", strerror(errno));
        return NULL;
    }

    size_t len = strlen(key_material);
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, key_material + written, len - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            set_error(CUTILS_ERR_IO, "write key: %s", strerror(errno));
            close(fd);
            unlink(template);
            return NULL;
        }
        written += (size_t)n;
    }
    close(fd);

    return strdup(template);
}

/* Fire the shutdown action (SSH or command). Does NOT wait for confirmation. */
int fire_target_action(const char *method, const char *host,
                       const char *username, const char *credential,
                       const char *command)
{
    if (strcmp(method, "ssh_password") == 0) {
        char cmd[2048];
        snprintf(cmd, sizeof(cmd),
                 "sshpass -p '%s' ssh "
                 "-o PreferredAuthentications=password "
                 "-o PubkeyAuthentication=no "
                 "-o StrictHostKeyChecking=no "
                 "-o ConnectTimeout=5 %s@%s %s",
                 credential, username, host, command);
        return run_shell(cmd) == 0 ? 0 : -1;

    } else if (strcmp(method, "ssh_key") == 0) {
        char *key_path = write_key_to_tmpfs(credential);
        if (!key_path) return -1;

        char cmd[2048];
        snprintf(cmd, sizeof(cmd),
                 "ssh -i '%s' "
                 "-o IdentitiesOnly=yes "
                 "-o BatchMode=yes "
                 "-o PreferredAuthentications=publickey "
                 "-o StrictHostKeyChecking=no "
                 "-o ConnectTimeout=5 %s@%s %s",
                 key_path, username, host, command);
        int rc = run_shell(cmd);

        unlink(key_path);
        free(key_path);
        return rc == 0 ? 0 : -1;

    } else if (strcmp(method, "command") == 0) {
        return run_shell(command) == 0 ? 0 : -1;
    }

    return set_error(CUTILS_ERR_INVALID, "unknown shutdown method: %s", method);
}

/* Execute a full target cycle: fire action, confirm down, post-delay. */
static int execute_target(const char *method, const char *host,
                          const char *username, const char *credential,
                          const char *command, int timeout,
                          const char *confirm_method, int confirm_port,
                          const char *confirm_cmd, int post_delay)
{
    int rc = fire_target_action(method, host, username, credential, command);
    if (rc != 0) return -1;

    rc = confirm_target_down(confirm_method, host, confirm_port,
                             confirm_cmd, timeout);
    if (rc != 0)
        log_warn("target confirmation timed out (%s:%s)", host ? host : "-",
                 confirm_method ? confirm_method : "ping");

    if (post_delay > 0)
        sleep((unsigned)post_delay);

    return 0;
}

static int test_target_connectivity(const char *method, const char *host,
                                    const char *username, const char *credential)
{
    if (strcmp(method, "ssh_password") == 0) {
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
                 "sshpass -p '%s' ssh -o StrictHostKeyChecking=no "
                 "-o ConnectTimeout=5 %s@%s 'echo OK'",
                 credential, username, host);
        return run_shell(cmd) == 0 ? 0 : -1;

    } else if (strcmp(method, "ssh_key") == 0) {
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
                 "ssh -i '%s' -o StrictHostKeyChecking=no "
                 "-o ConnectTimeout=5 %s@%s 'echo OK'",
                 credential, username, host);
        return run_shell(cmd) == 0 ? 0 : -1;

    } else if (strcmp(method, "command") == 0) {
        return 0; /* can't test arbitrary commands */
    }

    return -1;
}

/* --- Group execution with timeout --- */

/* Execute targets in parallel with optional group-level timeout.
 * Kills stragglers with SIGTERM if the group ceiling is reached.
 *
 * The forked child reports its target's pass/fail to the parent via its
 * exit status (0 = ok, 1 = failed) — the parent reads WEXITSTATUS on
 * waitpid and appends a result row. The child's specific error message
 * (cutils_get_error) lives only in its address space; the parent sees
 * "step failed (see daemon log)" and the journal carries the detail. */
static void execute_group_parallel(shutdown_mgr_t *mgr, const char *group_name,
                                   db_result_t *targets, int max_timeout,
                                   shutdown_result_t *res)
{
    int n = targets->nrows;
    pid_t *pids = calloc((size_t)n, sizeof(pid_t));
    if (!pids) {
        log_error("allocation failed for shutdown pids");
        return;
    }

    for (int t = 0; t < n; t++) {
        report(mgr, group_name, targets->rows[t][0], "starting");
        pids[t] = fork();
        if (pids[t] == 0) {
            int rc = execute_target(
                targets->rows[t][1], targets->rows[t][2],
                targets->rows[t][3], targets->rows[t][4],
                targets->rows[t][5], atoi(targets->rows[t][6]),
                targets->rows[t][7],                           /* confirm_method */
                targets->rows[t][8] ? atoi(targets->rows[t][8]) : 0, /* confirm_port */
                targets->rows[t][9],                           /* confirm_command */
                atoi(targets->rows[t][10]));                   /* post_confirm_delay */
            _exit(rc == 0 ? 0 : 1);
        }
    }

    /* Wait for all children, respecting group timeout */
    time_t start = time(NULL);
    int *done = calloc((size_t)n, sizeof(int));
    int *outcome = calloc((size_t)n, sizeof(int)); /* 0=ok 1=fail 2=killed */
    if (!done || !outcome) {
        log_error("allocation failed for done array");
        free(pids); free(done); free(outcome);
        return;
    }

    for (;;) {
        int all_done = 1;
        for (int t = 0; t < n; t++) {
            if (done[t] || pids[t] <= 0) continue;
            int status;
            pid_t w = waitpid(pids[t], &status, WNOHANG);
            if (w > 0) {
                done[t] = 1;
                int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
                outcome[t] = ok ? 0 : 1;
                report(mgr, group_name, targets->rows[t][0],
                       ok ? "completed" : "failed");
            } else {
                all_done = 0;
            }
        }
        if (all_done) break;

        if (max_timeout > 0 && time(NULL) - start >= max_timeout) {
            log_warn("group '%s' hit %ds ceiling, killing stragglers",
                     group_name, max_timeout);
            for (int t = 0; t < n; t++) {
                if (!done[t] && pids[t] > 0) {
                    kill(pids[t], SIGTERM);
                    outcome[t] = 2;
                    report(mgr, group_name, targets->rows[t][0], "killed (timeout)");
                }
            }
            /* Reap killed children */
            for (int t = 0; t < n; t++) {
                if (!done[t] && pids[t] > 0) {
                    waitpid(pids[t], NULL, 0);
                    done[t] = 1;
                }
            }
            break;
        }
        sleep(1);
    }

    /* Append result rows in target-order (post-execution to avoid racing
     * the accumulator with the polling loop above). */
    char step_target[96];
    for (int t = 0; t < n; t++) {
        snprintf(step_target, sizeof(step_target), "%s/%s",
                 group_name, targets->rows[t][0]);
        if (outcome[t] == 0)
            result_append(res, "phase1", step_target, 0, NULL);
        else if (outcome[t] == 2)
            result_append(res, "phase1", step_target, 1,
                          "killed: group timeout exceeded");
        else
            result_append(res, "phase1", step_target, 1,
                          "step failed (see daemon log)");
    }

    free(done);
    free(outcome);
    free(pids);
}

/* Execute targets sequentially with optional group-level timeout. */
static void execute_group_sequential(shutdown_mgr_t *mgr, const char *group_name,
                                     db_result_t *targets, int max_timeout,
                                     shutdown_result_t *res)
{
    time_t start = time(NULL);
    char step_target[96];

    for (int t = 0; t < targets->nrows; t++) {
        const char *tname = targets->rows[t][0];
        snprintf(step_target, sizeof(step_target), "%s/%s", group_name, tname);

        if (max_timeout > 0 && time(NULL) - start >= max_timeout) {
            log_warn("group '%s' hit %ds ceiling, skipping remaining targets",
                     group_name, max_timeout);
            result_append(res, "phase1", step_target, 2, NULL);
            continue;
        }

        report(mgr, group_name, tname, "starting");
        int ret = execute_target(
            targets->rows[t][1], targets->rows[t][2],
            targets->rows[t][3], targets->rows[t][4],
            targets->rows[t][5], atoi(targets->rows[t][6]),
            targets->rows[t][7],
            targets->rows[t][8] ? atoi(targets->rows[t][8]) : 0,
            targets->rows[t][9],
            atoi(targets->rows[t][10]));
        report(mgr, group_name, tname, ret == 0 ? "completed" : "failed");
        result_append(res, "phase1", step_target,
                      ret == 0 ? 0 : 1,
                      ret == 0 ? NULL : cutils_get_error());
    }
}

/* --- Public API --- */

shutdown_mgr_t *shutdown_create(cutils_db_t *db, ups_t *ups,
                                cutils_config_t *config)
{
    shutdown_mgr_t *mgr = calloc(1, sizeof(*mgr));
    if (!mgr) return NULL;
    mgr->db = db;
    mgr->ups = ups;
    mgr->config = config;
    return mgr;
}

void shutdown_free(shutdown_mgr_t *mgr)
{
    free(mgr);
}

void shutdown_on_progress(shutdown_mgr_t *mgr, shutdown_progress_fn fn,
                          void *userdata)
{
    mgr->progress_fn = fn;
    mgr->progress_ud = userdata;
}

int shutdown_execute(shutdown_mgr_t *mgr, int dry_run)
{
    return shutdown_execute_ex(mgr, dry_run, NULL);
}

int shutdown_execute_ex(shutdown_mgr_t *mgr, int dry_run,
                        shutdown_result_t **out)
{
    log_info("=== SHUTDOWN WORKFLOW %s ===", dry_run ? "(DRY RUN)" : "STARTED");
    time_t start = time(NULL);

    shutdown_result_t *res = NULL;
    if (out) {
        res = calloc(1, sizeof(*res));
        if (!res) {
            log_error("allocation failed for shutdown result");
            /* Continue without accumulating — workflow correctness wins
             * over reporting fidelity. */
        }
    }

    /* --- Phase 1: User-defined groups --- */

    CUTILS_AUTO_DBRES db_result_t *groups = NULL;
    int rc = db_execute(mgr->db,
        "SELECT id, name, parallel, max_timeout_sec, post_group_delay "
        "FROM shutdown_groups ORDER BY execution_order",
        NULL, &groups);

    if (rc != CUTILS_OK || !groups) {
        log_warn("no shutdown groups configured");
    } else {
        for (int g = 0; g < groups->nrows; g++) {
            const char *group_id   = groups->rows[g][0];
            const char *group_name = groups->rows[g][1];
            int parallel           = atoi(groups->rows[g][2]);
            int max_timeout        = atoi(groups->rows[g][3]);
            int post_group_delay   = atoi(groups->rows[g][4]);

            log_info("Group '%s' (%s, timeout=%ds, post_delay=%ds)",
                     group_name, parallel ? "parallel" : "sequential",
                     max_timeout, post_group_delay);

            const char *tparams[] = { group_id, NULL };
            CUTILS_AUTO_DBRES db_result_t *targets = NULL;
            rc = db_execute(mgr->db,
                "SELECT name, method, host, username, credential, command, "
                "timeout_sec, confirm_method, confirm_port, confirm_command, "
                "post_confirm_delay "
                "FROM shutdown_targets WHERE group_id = ? ORDER BY order_in_group",
                tparams, &targets);

            if (rc != CUTILS_OK || !targets || targets->nrows == 0) {
                log_warn("group '%s' has no targets — skipping", group_name);
                continue;
            }

            if (dry_run) {
                /* Pre-flight rows; commit-6 replaces these with real
                 * connectivity checks via test_target_connectivity. */
                char step_target[96];
                for (int t = 0; t < targets->nrows; t++) {
                    snprintf(step_target, sizeof(step_target), "%s/%s",
                             group_name, targets->rows[t][0]);
                    report(mgr, group_name, targets->rows[t][0], "would execute");
                    result_append(res, "phase1", step_target, 0, NULL);
                }
            } else if (parallel) {
                execute_group_parallel(mgr, group_name, targets, max_timeout, res);
            } else {
                execute_group_sequential(mgr, group_name, targets, max_timeout, res);
            }

            if (post_group_delay > 0 && !dry_run) {
                log_info("Group '%s' complete, waiting %ds", group_name, post_group_delay);
                sleep((unsigned)post_group_delay);
            }
        }
    }

    /* --- Phase 2: UPS action --- */

    const char *ups_mode = config_get_str(mgr->config, "shutdown.ups_mode");
    if (!ups_mode) ups_mode = "command";
    int ups_delay = config_get_int(mgr->config, "shutdown.ups_delay", 5);

    if (strcmp(ups_mode, "none") == 0) {
        log_info("UPS action: none (skipped)");
        result_append(res, "phase2", "ups", 2, NULL);
    } else if (strcmp(ups_mode, "command") == 0) {
        if (dry_run) {
            log_info("UPS action: would send shutdown command");
            result_append(res, "phase2", "ups", 0, NULL);
        } else {
            log_info("UPS action: sending shutdown command");
            const ups_cmd_desc_t *sd = ups_find_command_flag(mgr->ups, UPS_CMD_IS_SHUTDOWN);
            if (sd && ups_cmd_execute(mgr->ups, sd->name, 0) == 0) {
                log_info("UPS shutdown command accepted");
                result_append(res, "phase2", "ups", 0, NULL);
            } else {
                log_error("UPS shutdown command FAILED");
                result_append(res, "phase2", "ups", 1,
                              sd ? cutils_get_error()
                                 : "UPS shutdown command not supported by driver");
            }
        }
    } else if (strcmp(ups_mode, "register") == 0) {
        const char *reg_name = config_get_str(mgr->config, "shutdown.ups_register");
        int raw_val = config_get_int(mgr->config, "shutdown.ups_value", 0);
        if (dry_run) {
            log_info("UPS action: would write %d to register '%s'",
                     raw_val, reg_name ? reg_name : "");
            result_append(res, "phase2", "ups",
                          (reg_name && reg_name[0]) ? 0 : 1,
                          (reg_name && reg_name[0]) ? NULL
                              : "shutdown.ups_register is empty");
        } else if (reg_name && reg_name[0]) {
            log_info("UPS action: writing %d to register '%s'", raw_val, reg_name);
            const ups_config_reg_t *reg = ups_find_config_reg(mgr->ups, reg_name);
            if (reg && reg->writable) {
                if (ups_config_write(mgr->ups, reg, (uint16_t)raw_val) == 0) {
                    log_info("UPS register write accepted");
                    result_append(res, "phase2", "ups", 0, NULL);
                } else {
                    log_error("UPS register write FAILED");
                    result_append(res, "phase2", "ups", 1, cutils_get_error());
                }
            } else {
                log_error("UPS register '%s' not found or not writable", reg_name);
                result_append(res, "phase2", "ups", 1,
                              "register not found or not writable");
            }
        } else {
            log_error("UPS action mode is 'register' but no register name configured");
            result_append(res, "phase2", "ups", 1,
                          "shutdown.ups_register is empty");
        }
    } else {
        log_warn("unknown UPS action mode: '%s'", ups_mode);
        result_append(res, "phase2", "ups", 1, "unknown shutdown.ups_mode");
    }

    if (ups_delay > 0 && strcmp(ups_mode, "none") != 0 && !dry_run)
        sleep((unsigned)ups_delay);

    /* --- Phase 3: Controller shutdown --- */

    int ctrl_enabled = config_get_int(mgr->config, "shutdown.controller_enabled", 1);
#ifdef VERSION_STRING
    /* Docker builds stamp ".docker.<arch>" into VERSION_STRING via the
     * BUILD_TAG plumbing. A container can't power off its host kernel —
     * `systemctl poweroff` would either no-op or kill only the container.
     * Hard-skip Phase 3 regardless of shutdown.controller_enabled so a
     * misconfigured container can't surprise an operator. */
    int is_docker_build = (strstr(VERSION_STRING, ".docker.") != NULL);
#else
    int is_docker_build = 0;
#endif
    if (is_docker_build) {
        log_info("Controller: skipped (docker build cannot power off host)");
        result_append(res, "phase3", "controller", 2, NULL);
    } else if (ctrl_enabled) {
        if (dry_run) {
            log_info("Controller: would execute 'systemctl poweroff'");
            result_append(res, "phase3", "controller", 0, NULL);
        } else {
            log_info("Controller: shutting down via systemctl poweroff");
            /* Goes through logind → systemd, gracefully stops services and
             * syncs filesystems. The .deb ships a polkit rule granting the
             * airies-ups system user org.freedesktop.login1.power-off; no
             * sudo, no setuid binary, no root. */
            int shut_rc = run_shell("systemctl poweroff");
            if (shut_rc != 0) {
                log_warn("controller shutdown command returned %d", shut_rc);
                char buf[64];
                snprintf(buf, sizeof(buf), "systemctl poweroff exited %d", shut_rc);
                result_append(res, "phase3", "controller", 1, buf);
            } else {
                result_append(res, "phase3", "controller", 0, NULL);
            }
        }
    } else {
        log_info("Controller: shutdown disabled (skipped)");
        result_append(res, "phase3", "controller", 2, NULL);
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "=== SHUTDOWN WORKFLOW COMPLETE in %lds ===",
             time(NULL) - start);
    log_info("%s", msg);

    if (out) *out = res;
    return CUTILS_OK;
}

/* --- Trigger evaluation --- */

/* Look up a numeric field in ups_data by name. Returns 0 if not found. */
static double get_ups_field(const ups_data_t *data, const char *name)
{
    if (!name || !name[0]) return 0;
    if (strcmp(name, "runtime_sec") == 0)       return (double)data->runtime_sec;
    if (strcmp(name, "charge_pct") == 0)        return data->charge_pct;
    if (strcmp(name, "battery_voltage") == 0)   return data->battery_voltage;
    if (strcmp(name, "load_pct") == 0)          return data->load_pct;
    if (strcmp(name, "output_current") == 0)    return data->output_current;
    if (strcmp(name, "output_voltage") == 0)    return data->output_voltage;
    if (strcmp(name, "output_frequency") == 0)  return data->output_frequency;
    if (strcmp(name, "input_voltage") == 0)     return data->input_voltage;
    if (strcmp(name, "efficiency") == 0) {
        /* efficiency is only meaningful when the reason is UPS_EFF_OK.
         * Return NaN otherwise so compare_field treats it as "no trigger." */
        return data->efficiency_reason == UPS_EFF_OK
               ? data->efficiency
               : (0.0 / 0.0);
    }
    if (strcmp(name, "bypass_voltage") == 0)    return data->bypass_voltage;
    if (strcmp(name, "bypass_frequency") == 0)  return data->bypass_frequency;
    return 0;
}

static int compare_field(double value, const char *op, double threshold)
{
    if (!op || strcmp(op, "lt") == 0) return value < threshold;
    if (strcmp(op, "gt") == 0)        return value > threshold;
    if (strcmp(op, "eq") == 0)        return value == threshold;
    return 0;
}

void shutdown_check_trigger(shutdown_mgr_t *mgr, const ups_data_t *data)
{
    if (mgr->triggered) return;

    const char *mode = config_get_str(mgr->config, "shutdown.trigger");
    if (!mode) mode = "software";

    /* Manual mode — never auto-trigger */
    if (strcmp(mode, "manual") == 0) return;

    int condition_met = 0;
    int delay_sec = config_get_int(mgr->config, "shutdown.trigger_delay_sec", 30);

    if (strcmp(mode, "ups") == 0) {
        /* Automatic - UPS: defer entirely to the UPS's shutdown-imminent flag */
        if (data->sig_status & UPS_SIG_SHUTDOWN_IMMINENT)
            condition_met = 1;

    } else {
        /* Automatic - Software: we own the decision */
        int require_battery = config_get_int(mgr->config, "shutdown.trigger_on_battery", 1);
        int on_battery = (data->status & UPS_ST_ON_BATTERY) != 0;
        if (require_battery && !on_battery) {
            mgr->trigger_first_met = 0;
            return;
        }

        const char *source = config_get_str(mgr->config, "shutdown.trigger_source");
        if (!source) source = "runtime";

        if (strcmp(source, "runtime") == 0) {
            /* Runtime / battery percentage thresholds */
            int runtime_thresh = config_get_int(mgr->config, "shutdown.trigger_runtime_sec", 300);
            int battery_thresh = config_get_int(mgr->config, "shutdown.trigger_battery_pct", 0);

            if (runtime_thresh > 0 && data->runtime_sec > 0 &&
                data->runtime_sec <= (uint32_t)runtime_thresh)
                condition_met = 1;

            if (battery_thresh > 0 && data->charge_pct <= (double)battery_thresh)
                condition_met = 1;

        } else if (strcmp(source, "field") == 0) {
            /* Arbitrary data field watch */
            const char *field = config_get_str(mgr->config, "shutdown.trigger_field");
            if (field && field[0]) {
                const char *op = config_get_str(mgr->config, "shutdown.trigger_field_op");
                int thresh_val = config_get_int(mgr->config, "shutdown.trigger_field_value", 0);
                double actual = get_ups_field(data, field);
                if (compare_field(actual, op, (double)thresh_val))
                    condition_met = 1;
            }
        }
    }

    if (!condition_met) {
        mgr->trigger_first_met = 0;
        return;
    }

    /* Debounce */
    time_t now = time(NULL);
    if (mgr->trigger_first_met == 0) {
        mgr->trigger_first_met = now;
        log_warn("shutdown trigger condition met, debouncing %ds", delay_sec);
        return;
    }

    if (now - mgr->trigger_first_met < delay_sec)
        return;

    /* Fire */
    log_error("SHUTDOWN TRIGGER: conditions held for %lds — executing workflow",
              now - mgr->trigger_first_met);
    mgr->triggered = 1;
    shutdown_execute(mgr, 0);
}

int shutdown_test_target(shutdown_mgr_t *mgr, const char *target_name)
{
    const char *params[] = { target_name, NULL };
    CUTILS_AUTO_DBRES db_result_t *result = NULL;

    int rc = db_execute(mgr->db,
        "SELECT method, host, username, credential FROM shutdown_targets WHERE name = ?",
        params, &result);

    if (rc != CUTILS_OK || !result || result->nrows == 0)
        return set_error(CUTILS_ERR_NOT_FOUND, "target '%s' not found", target_name);

    const char *method     = result->rows[0][0];
    const char *host       = result->rows[0][1];
    const char *username   = result->rows[0][2];
    const char *credential = result->rows[0][3];

    log_info("testing target '%s' (%s -> %s@%s)", target_name, method, username, host);
    rc = test_target_connectivity(method, host, username, credential);

    if (rc == 0)
        log_info("target '%s': connectivity OK", target_name);
    else
        log_error("target '%s': connectivity FAILED", target_name);

    return rc;
}
