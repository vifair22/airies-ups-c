#include "shutdown/shutdown.h"
#include <cutils/db.h>
#include <cutils/log.h>
#include <cutils/error.h>
#include <cutils/mem.h>

/* config_get_str for DB-backed keys returns a pointer into a shared static
 * buffer (db_val_buf in c-utils/src/config.c). Any subsequent config_get_*
 * call on another DB-backed key overwrites that buffer, silently clobbering
 * the previously-returned pointer. Callers that need to hold onto a string
 * across more than one config read MUST snapshot it into a local buffer
 * first — that's what this helper does. */
static void cfg_get_str_copy(const cutils_config_t *cfg, const char *key,
                             char *buf, size_t bufsz)
{
    const char *v = config_get_str(cfg, key);
    snprintf(buf, bufsz, "%s", v ? v : "");
}

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
    return system(cmd) == 0;
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
        if (system(cmd) == 0)
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

/* Fire the shutdown action (SSH or command). Does NOT wait for confirmation. */
static int fire_target_action(const char *method, const char *host,
                              const char *username, const char *credential,
                              const char *command)
{
    if (strcmp(method, "ssh_password") == 0) {
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
                 "sshpass -p '%s' ssh -o StrictHostKeyChecking=no "
                 "-o ConnectTimeout=5 %s@%s %s",
                 credential, username, host, command);
        return system(cmd) == 0 ? 0 : -1;

    } else if (strcmp(method, "ssh_key") == 0) {
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
                 "ssh -i '%s' -o StrictHostKeyChecking=no "
                 "-o ConnectTimeout=5 %s@%s %s",
                 credential, username, host, command);
        return system(cmd) == 0 ? 0 : -1;

    } else if (strcmp(method, "command") == 0) {
        return system(command) == 0 ? 0 : -1;
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
        return system(cmd) == 0 ? 0 : -1;

    } else if (strcmp(method, "ssh_key") == 0) {
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
                 "ssh -i '%s' -o StrictHostKeyChecking=no "
                 "-o ConnectTimeout=5 %s@%s 'echo OK'",
                 credential, username, host);
        return system(cmd) == 0 ? 0 : -1;

    } else if (strcmp(method, "command") == 0) {
        return 0; /* can't test arbitrary commands */
    }

    return -1;
}

/* --- Group execution with timeout --- */

/* Execute targets in parallel with optional group-level timeout.
 * Kills stragglers with SIGTERM if the group ceiling is reached. */
static void execute_group_parallel(shutdown_mgr_t *mgr, const char *group_name,
                                   db_result_t *targets, int max_timeout)
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
            execute_target(
                targets->rows[t][1], targets->rows[t][2],
                targets->rows[t][3], targets->rows[t][4],
                targets->rows[t][5], atoi(targets->rows[t][6]),
                targets->rows[t][7],                           /* confirm_method */
                targets->rows[t][8] ? atoi(targets->rows[t][8]) : 0, /* confirm_port */
                targets->rows[t][9],                           /* confirm_command */
                atoi(targets->rows[t][10]));                   /* post_confirm_delay */
            _exit(0);
        }
    }

    /* Wait for all children, respecting group timeout */
    time_t start = time(NULL);
    int *done = calloc((size_t)n, sizeof(int));
    if (!done) {
        log_error("allocation failed for done array");
        free(pids);
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
                report(mgr, group_name, targets->rows[t][0],
                       WIFEXITED(status) && WEXITSTATUS(status) == 0
                       ? "completed" : "failed");
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
                    report(mgr, group_name, targets->rows[t][0], "killed (timeout)");
                }
            }
            /* Reap killed children */
            for (int t = 0; t < n; t++) {
                if (!done[t] && pids[t] > 0)
                    waitpid(pids[t], NULL, 0);
            }
            break;
        }
        sleep(1);
    }

    free(done);
    free(pids);
}

/* Execute targets sequentially with optional group-level timeout. */
static void execute_group_sequential(shutdown_mgr_t *mgr, const char *group_name,
                                     db_result_t *targets, int max_timeout)
{
    time_t start = time(NULL);

    for (int t = 0; t < targets->nrows; t++) {
        if (max_timeout > 0 && time(NULL) - start >= max_timeout) {
            log_warn("group '%s' hit %ds ceiling, skipping remaining targets",
                     group_name, max_timeout);
            break;
        }

        report(mgr, group_name, targets->rows[t][0], "starting");
        int ret = execute_target(
            targets->rows[t][1], targets->rows[t][2],
            targets->rows[t][3], targets->rows[t][4],
            targets->rows[t][5], atoi(targets->rows[t][6]),
            targets->rows[t][7],
            targets->rows[t][8] ? atoi(targets->rows[t][8]) : 0,
            targets->rows[t][9],
            atoi(targets->rows[t][10]));
        report(mgr, group_name, targets->rows[t][0],
               ret == 0 ? "completed" : "failed");
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
    log_info("=== SHUTDOWN WORKFLOW %s ===", dry_run ? "(DRY RUN)" : "STARTED");
    time_t start = time(NULL);

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

            if (rc != CUTILS_OK || !targets || targets->nrows == 0) continue;

            if (dry_run) {
                for (int t = 0; t < targets->nrows; t++)
                    report(mgr, group_name, targets->rows[t][0], "would execute");
            } else if (parallel) {
                execute_group_parallel(mgr, group_name, targets, max_timeout);
            } else {
                execute_group_sequential(mgr, group_name, targets, max_timeout);
            }

            if (post_group_delay > 0 && !dry_run) {
                log_info("Group '%s' complete, waiting %ds", group_name, post_group_delay);
                sleep((unsigned)post_group_delay);
            }
        }
    }

    /* --- Phase 2: UPS action --- */

    char ups_mode[32];
    cfg_get_str_copy(mgr->config, "shutdown.ups_mode", ups_mode, sizeof(ups_mode));
    if (!ups_mode[0]) snprintf(ups_mode, sizeof(ups_mode), "%s", "command");
    int ups_delay = config_get_int(mgr->config, "shutdown.ups_delay", 5);

    if (strcmp(ups_mode, "none") == 0) {
        log_info("UPS action: none (skipped)");
    } else if (strcmp(ups_mode, "command") == 0) {
        if (dry_run) {
            log_info("UPS action: would send shutdown command");
        } else {
            log_info("UPS action: sending shutdown command");
            const ups_cmd_desc_t *sd = ups_find_command_flag(mgr->ups, UPS_CMD_IS_SHUTDOWN);
            if (sd && ups_cmd_execute(mgr->ups, sd->name, 0) == 0)
                log_info("UPS shutdown command accepted");
            else
                log_error("UPS shutdown command FAILED");
        }
    } else if (strcmp(ups_mode, "register") == 0) {
        char reg_name[64];
        cfg_get_str_copy(mgr->config, "shutdown.ups_register", reg_name, sizeof(reg_name));
        int raw_val = config_get_int(mgr->config, "shutdown.ups_value", 0);
        if (dry_run) {
            log_info("UPS action: would write %d to register '%s'", raw_val, reg_name);
        } else if (reg_name[0]) {
            log_info("UPS action: writing %d to register '%s'", raw_val, reg_name);
            const ups_config_reg_t *reg = ups_find_config_reg(mgr->ups, reg_name);
            if (reg && reg->writable) {
                if (ups_config_write(mgr->ups, reg, (uint16_t)raw_val) == 0)
                    log_info("UPS register write accepted");
                else
                    log_error("UPS register write FAILED");
            } else {
                log_error("UPS register '%s' not found or not writable", reg_name);
            }
        } else {
            log_error("UPS action mode is 'register' but no register name configured");
        }
    } else {
        log_warn("unknown UPS action mode: '%s'", ups_mode);
    }

    if (ups_delay > 0 && strcmp(ups_mode, "none") != 0 && !dry_run)
        sleep((unsigned)ups_delay);

    /* --- Phase 3: Controller shutdown --- */

    int ctrl_enabled = config_get_int(mgr->config, "shutdown.controller_enabled", 1);
    if (ctrl_enabled) {
        if (dry_run) {
            log_info("Controller: would execute 'sudo shutdown -h now'");
        } else {
            log_info("Controller: shutting down");
            int shut_rc = system("sudo shutdown -h now");
            if (shut_rc != 0)
                log_warn("controller shutdown command returned %d", shut_rc);
        }
    } else {
        log_info("Controller: shutdown disabled (skipped)");
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "=== SHUTDOWN WORKFLOW COMPLETE in %lds ===",
             time(NULL) - start);
    log_info("%s", msg);

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

    char mode[32];
    cfg_get_str_copy(mgr->config, "shutdown.trigger", mode, sizeof(mode));
    if (!mode[0]) snprintf(mode, sizeof(mode), "%s", "software");

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

        char source[32];
        cfg_get_str_copy(mgr->config, "shutdown.trigger_source", source, sizeof(source));
        if (!source[0]) snprintf(source, sizeof(source), "%s", "runtime");

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
            char field[32];
            cfg_get_str_copy(mgr->config, "shutdown.trigger_field", field, sizeof(field));
            if (field[0]) {
                char op[8];
                cfg_get_str_copy(mgr->config, "shutdown.trigger_field_op", op, sizeof(op));
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
