#include "shutdown/shutdown.h"
#include <cutils/log.h>
#include <cutils/error.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>

struct shutdown_mgr {
    cutils_db_t        *db;
    ups_t              *ups;
    shutdown_progress_fn progress_fn;
    void               *progress_ud;
};

/* --- Helpers --- */

static void report(shutdown_mgr_t *mgr, const char *group,
                   const char *target, const char *status)
{
    log_info("[%s/%s] %s", group, target, status);
    if (mgr->progress_fn)
        mgr->progress_fn(group, target, status, mgr->progress_ud);
}

static int is_host_online(const char *host)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ping -c 1 -W 1 %s >/dev/null 2>&1", host);
    return system(cmd) == 0;
}

static void wait_for_host_down(const char *host, int timeout_sec)
{
    time_t start = time(NULL);
    while (is_host_online(host)) {
        if (time(NULL) - start >= timeout_sec)
            return;
        sleep(5);
    }
    sleep(15); /* grace period */
}

static int execute_target(const char *method, const char *host,
                          const char *username, const char *credential,
                          const char *command, int timeout)
{
    if (strcmp(method, "ssh_password") == 0) {
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
                 "sshpass -p '%s' ssh -o StrictHostKeyChecking=no "
                 "-o ConnectTimeout=5 %s@%s %s",
                 credential, username, host, command);
        int ret = system(cmd);
        if (ret != 0) return -1;
        wait_for_host_down(host, timeout);
        return 0;

    } else if (strcmp(method, "ssh_key") == 0) {
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
                 "ssh -i '%s' -o StrictHostKeyChecking=no "
                 "-o ConnectTimeout=5 %s@%s %s",
                 credential, username, host, command);
        int ret = system(cmd);
        if (ret != 0) return -1;
        wait_for_host_down(host, timeout);
        return 0;

    } else if (strcmp(method, "command") == 0) {
        int ret = system(command);
        return ret == 0 ? 0 : -1;
    }

    return set_error(CUTILS_ERR_INVALID, "unknown shutdown method: %s", method);
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

/* --- Public API --- */

shutdown_mgr_t *shutdown_create(cutils_db_t *db, ups_t *ups)
{
    shutdown_mgr_t *mgr = calloc(1, sizeof(*mgr));
    if (!mgr) return NULL;
    mgr->db = db;
    mgr->ups = ups;
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

int shutdown_execute(shutdown_mgr_t *mgr, int dry_run,
                     int skip_ups, int skip_self)
{
    log_info("=== SHUTDOWN WORKFLOW %s ===", dry_run ? "(DRY RUN)" : "STARTED");
    time_t start = time(NULL);

    /* Query groups ordered by execution_order */
    db_result_t *groups = NULL;
    int rc = db_execute(mgr->db,
        "SELECT id, name, parallel FROM shutdown_groups ORDER BY execution_order",
        NULL, &groups);

    if (rc != CUTILS_OK || !groups) {
        log_warn("no shutdown groups configured");
    } else {
        for (int g = 0; g < groups->nrows; g++) {
            const char *group_id = groups->rows[g][0];
            const char *group_name = groups->rows[g][1];
            int parallel = atoi(groups->rows[g][2]);

            log_info("Group '%s' (%s)", group_name,
                     parallel ? "parallel" : "sequential");

            /* Query targets for this group */
            const char *tparams[] = { group_id, NULL };
            db_result_t *targets = NULL;
            rc = db_execute(mgr->db,
                "SELECT name, method, host, username, credential, command, timeout_sec "
                "FROM shutdown_targets WHERE group_id = ? ORDER BY order_in_group",
                tparams, &targets);

            if (rc != CUTILS_OK || !targets || targets->nrows == 0) {
                db_result_free(targets);
                continue;
            }

            if (dry_run) {
                for (int t = 0; t < targets->nrows; t++)
                    report(mgr, group_name, targets->rows[t][0], "would execute");
                db_result_free(targets);
                continue;
            }

            if (parallel) {
                /* Fork per target */
                pid_t *pids = calloc((size_t)targets->nrows, sizeof(pid_t));
                if (!pids) {
                    log_error("allocation failed for shutdown pids");
                    db_result_free(targets);
                    continue;
                }
                for (int t = 0; t < targets->nrows; t++) {
                    report(mgr, group_name, targets->rows[t][0], "starting");
                    pids[t] = fork();
                    if (pids[t] == 0) {
                        /* Child */
                        execute_target(targets->rows[t][1], targets->rows[t][2],
                                       targets->rows[t][3], targets->rows[t][4],
                                       targets->rows[t][5], atoi(targets->rows[t][6]));
                        _exit(0);
                    }
                }
                /* Wait for all children */
                for (int t = 0; t < targets->nrows; t++) {
                    if (pids[t] > 0) {
                        int status;
                        waitpid(pids[t], &status, 0);
                        report(mgr, group_name, targets->rows[t][0],
                               WIFEXITED(status) && WEXITSTATUS(status) == 0
                               ? "completed" : "failed");
                    }
                }
                free(pids);
            } else {
                /* Sequential */
                for (int t = 0; t < targets->nrows; t++) {
                    report(mgr, group_name, targets->rows[t][0], "starting");
                    int ret = execute_target(
                        targets->rows[t][1], targets->rows[t][2],
                        targets->rows[t][3], targets->rows[t][4],
                        targets->rows[t][5], atoi(targets->rows[t][6]));
                    report(mgr, group_name, targets->rows[t][0],
                           ret == 0 ? "completed" : "failed");
                }
            }

            db_result_free(targets);
        }
    }
    db_result_free(groups);

    /* Final phase: UPS shutdown command */
    if (!skip_ups) {
        if (dry_run) {
            log_info("UPS shutdown: would send command");
        } else {
            log_info("Sending UPS shutdown command");
            if (ups_cmd_shutdown(mgr->ups) == 0)
                log_info("UPS shutdown command accepted");
            else
                log_error("UPS shutdown command FAILED");
            sleep(5);
        }
    } else {
        log_warn("UPS shutdown: SKIPPED");
    }

    /* Final phase: self-shutdown */
    if (!skip_self) {
        if (dry_run) {
            log_info("Self shutdown: would execute 'sudo shutdown -h now'");
        } else {
            log_info("Shutting down this host");
            int shut_rc = system("sudo shutdown -h now");
            if (shut_rc != 0)
                log_warn("self-shutdown command returned %d", shut_rc);
        }
    } else {
        log_warn("Self shutdown: SKIPPED");
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "=== SHUTDOWN WORKFLOW COMPLETE in %lds ===",
             time(NULL) - start);
    log_info("%s", msg);

    return CUTILS_OK;
}

int shutdown_test_target(shutdown_mgr_t *mgr, const char *target_name)
{
    const char *params[] = { target_name, NULL };
    db_result_t *result = NULL;

    int rc = db_execute(mgr->db,
        "SELECT method, host, username, credential FROM shutdown_targets WHERE name = ?",
        params, &result);

    if (rc != CUTILS_OK || !result || result->nrows == 0) {
        db_result_free(result);
        return set_error(CUTILS_ERR_NOT_FOUND, "target '%s' not found", target_name);
    }

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

    db_result_free(result);
    return rc;
}
