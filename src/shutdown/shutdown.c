#include "shutdown/shutdown.h"
#include <cutils/db.h>
#include <cutils/log.h>
#include <cutils/error.h>
#include <cutils/mem.h>

#include <pthread.h>
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
    monitor_t          *monitor;            /* optional event sink (NULL = no-op) */
    shutdown_progress_fn progress_fn;
    void               *progress_ud;

    /* Trigger state */
    time_t              trigger_first_met;  /* when condition first became true (0=not met) */
    int                 triggered;          /* 1 = workflow already fired, don't re-trigger */

    /* Async workflow state — all fields below are guarded by state_lock.
     * The worker thread mirrors per-step results into `snapshot` (under
     * lock) so a polled reader sees progress as it happens, not just at
     * the end. `worker` is joinable iff `worker_started`. */
    pthread_mutex_t     state_lock;
    pthread_t           worker;
    int                 worker_started;
    shutdown_state_t    state;
    uint64_t            workflow_id;
    int                 active_dry_run;
    time_t              started_at;
    time_t              finished_at;
    char                current_phase[16];
    char                current_target[96];
    shutdown_result_t  *snapshot;           /* mgr-owned mirror of the worker's res */
};

/* Mirror a workflow milestone into the events table. NULL monitor is the
 * no-op path used by unit tests; production always has a monitor wired
 * via main.c. The events table INSERT inside monitor_fire_event is
 * synchronous, so milestones land before subsequent phases run — this
 * matters for the controller-poweroff path, where the daemon may be
 * SIGTERM'd by systemd shortly after returning. */
static void emit_event(shutdown_mgr_t *mgr, const char *severity,
                       const char *title, const char *message)
{
    if (!mgr || !mgr->monitor) return;
    monitor_fire_event(mgr->monitor, severity, "shutdown", title, message);
}

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

/* Append a row to a result accumulator. Out-of-memory drops the row but
 * keeps the workflow going — reporting fidelity is not worth aborting
 * a real shutdown sequence over. */
static void result_row_fill(shutdown_step_result_t *row, const char *phase,
                            const char *target, int ok, const char *err)
{
    memset(row, 0, sizeof(*row));
    snprintf(row->phase,  sizeof(row->phase),  "%s", phase  ? phase  : "");
    snprintf(row->target, sizeof(row->target), "%s", target ? target : "");
    row->ok = ok;
    if (ok == 1)
        snprintf(row->error, sizeof(row->error), "%s", err ? err : "");
}

static void result_append_to(shutdown_result_t *res, const char *phase,
                             const char *target, int ok, const char *err)
{
    if (!res) return;
    size_t cap = res->n_steps + 1;
    shutdown_step_result_t *grow = realloc(res->steps, cap * sizeof(*grow));
    if (!grow) return;
    res->steps = grow;
    result_row_fill(&res->steps[res->n_steps], phase, target, ok, err);
    res->n_steps++;
    if (ok == 1) res->n_failed++;
}

/* Append a step row to the worker's local accumulator AND mirror the
 * row into the manager's snapshot under state_lock so a concurrent
 * shutdown_get_status sees progress as it happens. The mirror also
 * updates current_phase / current_target so polled status carries a
 * "what's running right now" pointer.
 *
 * Synchronous test paths (which call shutdown_execute_ex directly with
 * mgr->state == SHUTDOWN_IDLE) skip the mirror — there's no live reader
 * to publish to, and bypassing the lock keeps those tests free of any
 * thread-safety dependencies. */
static void result_append(shutdown_mgr_t *mgr, shutdown_result_t *res,
                          const char *phase, const char *target,
                          int ok, const char *err)
{
    result_append_to(res, phase, target, ok, err);

    if (!mgr) return;
    pthread_mutex_lock(&mgr->state_lock);
    if (mgr->state == SHUTDOWN_RUNNING && mgr->snapshot) {
        result_append_to(mgr->snapshot, phase, target, ok, err);
        snprintf(mgr->current_phase,  sizeof(mgr->current_phase),
                 "%s", phase  ? phase  : "");
        snprintf(mgr->current_target, sizeof(mgr->current_target),
                 "%s", target ? target : "");
    }
    pthread_mutex_unlock(&mgr->state_lock);
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

/* Single-shot TCP reachability check. Returns 1 if a connection can be
 * established within ~2 seconds, 0 if the port refuses, can't be resolved,
 * or the connect attempt times out. Used by both the wait-for-down poll
 * loop (confirm_tcp_port) and the per-target reachability probe
 * (test_target_confirm). */
static int tcp_port_open(const char *host, int port)
{
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints = { .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0) return 0;

    int fd = socket(res->ai_family, SOCK_STREAM, 0);
    if (fd < 0) { freeaddrinfo(res); return 0; }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int reachable = 0;
    int cr = connect(fd, res->ai_addr, res->ai_addrlen);
    if (cr == 0) {
        reachable = 1;
    } else if (errno == EINPROGRESS) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv = { .tv_sec = 2 };
        if (select(fd + 1, NULL, &wfds, NULL, &tv) > 0) {
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
            reachable = (err == 0);
        }
    }

    close(fd);
    freeaddrinfo(res);
    return reachable;
}

/* Wait for a TCP port to become unreachable (connect refused or timeout).
 * Returns 0 if confirmed down, -1 if still reachable at timeout. */
static int confirm_tcp_port(const char *host, int port, int timeout_sec)
{
    time_t start = time(NULL);
    while (tcp_port_open(host, port)) {
        if (time(NULL) - start >= timeout_sec)
            return -1;
        sleep(5);
    }
    return 0;
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
    if (!method) return -1;
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

/* Single-shot reachability probe over the configured down-detect method.
 * Inverse of confirm_target_down: validates the operator can OBSERVE the
 * target's current up state via the configured mechanism, so that during
 * a real run the wait-for-down loop has something to observe.
 *
 * Returns 0 if reachable (i.e. the down-detect mechanism would correctly
 * report "still up" right now), -1 if not. method="none" is trivially ok
 * since there's nothing to verify. */
static int test_target_confirm(const char *method, const char *host,
                               int port, const char *cmd)
{
    if (!method || strcmp(method, "none") == 0)
        return 0;
    if (strcmp(method, "ping") == 0)
        return is_host_online(host) ? 0 : -1;
    if (strcmp(method, "tcp_port") == 0)
        return tcp_port_open(host, port) ? 0 : -1;
    if (strcmp(method, "command") == 0 && cmd && cmd[0])
        return system(cmd) == 0 ? 0 : -1;
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
            result_append(mgr, res, "phase1", step_target, 0, NULL);
        else if (outcome[t] == 2)
            result_append(mgr, res, "phase1", step_target, 1,
                          "killed: group timeout exceeded");
        else
            result_append(mgr, res, "phase1", step_target, 1,
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
            result_append(mgr, res, "phase1", step_target, 2, NULL);
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
        result_append(mgr, res, "phase1", step_target,
                      ret == 0 ? 0 : 1,
                      ret == 0 ? NULL : cutils_get_error());
    }
}

/* --- Public API --- */

shutdown_mgr_t *shutdown_create(cutils_db_t *db, ups_t *ups,
                                cutils_config_t *config, monitor_t *mon)
{
    shutdown_mgr_t *mgr = calloc(1, sizeof(*mgr));
    if (!mgr) return NULL;
    mgr->db = db;
    mgr->ups = ups;
    mgr->config = config;
    mgr->monitor = mon;
    mgr->state = SHUTDOWN_IDLE;
    if (pthread_mutex_init(&mgr->state_lock, NULL) != 0) {
        free(mgr);
        return NULL;
    }
    return mgr;
}

void shutdown_free(shutdown_mgr_t *mgr)
{
    if (!mgr) return;

    /* Block until the worker has exited so its writes to mgr->snapshot
     * are visible — and so we don't free mgr out from under it. The
     * controller-poweroff path of a real shutdown never reaches here
     * (systemd SIGTERMs us first); this matters mostly for unit tests
     * and clean app shutdown. */
    pthread_mutex_lock(&mgr->state_lock);
    pthread_t   prior       = mgr->worker;
    int         prior_valid = mgr->worker_started;
    mgr->worker_started     = 0;
    pthread_mutex_unlock(&mgr->state_lock);
    if (prior_valid) pthread_join(prior, NULL);

    if (mgr->snapshot) {
        free(mgr->snapshot->steps);
        free(mgr->snapshot);
    }
    pthread_mutex_destroy(&mgr->state_lock);
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
    {
        char buf[96];
        snprintf(buf, sizeof(buf),
                 dry_run ? "Pre-flight validation across all phases"
                         : "Executing shutdown workflow");
        emit_event(mgr, dry_run ? "warning" : "critical",
                   dry_run ? "Dry Run Started" : "Shutdown Workflow Started",
                   buf);
    }
    time_t start = time(NULL);

    /* Always alloc the result accumulator — the per-phase event summaries
     * read n_failed deltas off it whether or not the caller asked for the
     * struct via `out`. Freed at end if `out` is NULL. */
    shutdown_result_t *res = calloc(1, sizeof(*res));
    if (!res) {
        log_error("allocation failed for shutdown result");
        /* Continue without accumulating — workflow correctness wins
         * over reporting fidelity. */
    }

    /* --- Phase 1: User-defined groups --- */
    size_t phase1_total       = 0;
    size_t phase1_failed_at_0 = res ? res->n_failed : 0;

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

            phase1_total += (size_t)targets->nrows;

            if (dry_run) {
                /* Per-target validation: round-trip an `echo OK` over the
                 * configured ssh method so we catch missing sshpass, bad
                 * credentials, host-down, and similar before the operator
                 * relies on the workflow during a real outage. */
                char step_target[96];
                for (int t = 0; t < targets->nrows; t++) {
                    const char *tname  = targets->rows[t][0];
                    const char *method = targets->rows[t][1];
                    const char *host   = targets->rows[t][2];
                    const char *user   = targets->rows[t][3];
                    const char *cred   = targets->rows[t][4];
                    snprintf(step_target, sizeof(step_target), "%s/%s",
                             group_name, tname);

                    int probe_rc = test_target_connectivity(method, host, user, cred);
                    if (probe_rc == 0) {
                        report(mgr, group_name, tname, "validated");
                        result_append(mgr, res, "phase1", step_target, 0, NULL);
                    } else {
                        report(mgr, group_name, tname, "validation FAILED");
                        /* method/host/user are NOT NULL per the schema;
                         * test_target_connectivity also guards against a
                         * null method up front. No need to ternary-defend
                         * around them — doing so confused cppcheck into
                         * thinking the call site might pass null. */
                        char err[256];
                        snprintf(err, sizeof(err),
                                 "%s probe to %s@%s failed", method, user, host);
                        result_append(mgr, res, "phase1", step_target, 1, err);
                    }
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

    /* Phase 1 summary event. Skip if no targets ran — emitting a "0 ok,
     * 0 failed" row would be noise on every workflow that has no Phase 1
     * groups configured. */
    if (phase1_total > 0) {
        size_t phase1_failed = res ? res->n_failed - phase1_failed_at_0 : 0;
        size_t phase1_ok     = phase1_total - phase1_failed;
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "%zu of %zu target%s succeeded",
                 phase1_ok, phase1_total, phase1_total == 1 ? "" : "s");
        emit_event(mgr, phase1_failed > 0 ? "error" : "info",
                   "Phase 1 Complete", buf);
    }

    /* --- Phase 2: UPS action --- */

    const char *ups_mode = config_get_str(mgr->config, "shutdown.ups_mode");
    if (!ups_mode) ups_mode = "command";
    int ups_delay = config_get_int(mgr->config, "shutdown.ups_delay", 5);
    size_t phase2_failed_at_0 = res ? res->n_failed : 0;

    if (strcmp(ups_mode, "none") == 0) {
        log_info("UPS action: none (skipped)");
        result_append(mgr, res, "phase2", "ups", 2, NULL);
    } else if (strcmp(ups_mode, "command") == 0) {
        if (dry_run) {
            const ups_cmd_desc_t *sd = ups_find_command_flag(mgr->ups, UPS_CMD_IS_SHUTDOWN);
            if (sd) {
                log_info("UPS action: would send '%s'", sd->name);
                result_append(mgr, res, "phase2", "ups", 0, NULL);
            } else {
                log_error("UPS action: driver advertises no shutdown command");
                result_append(mgr, res, "phase2", "ups", 1,
                              "driver advertises no shutdown command");
            }
        } else {
            log_info("UPS action: sending shutdown command");
            const ups_cmd_desc_t *sd = ups_find_command_flag(mgr->ups, UPS_CMD_IS_SHUTDOWN);
            if (sd && ups_cmd_execute(mgr->ups, sd->name, 0) == 0) {
                log_info("UPS shutdown command accepted");
                result_append(mgr, res, "phase2", "ups", 0, NULL);
            } else {
                log_error("UPS shutdown command FAILED");
                result_append(mgr, res, "phase2", "ups", 1,
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
            result_append(mgr, res, "phase2", "ups",
                          (reg_name && reg_name[0]) ? 0 : 1,
                          (reg_name && reg_name[0]) ? NULL
                              : "shutdown.ups_register is empty");
        } else if (reg_name && reg_name[0]) {
            log_info("UPS action: writing %d to register '%s'", raw_val, reg_name);
            const ups_config_reg_t *reg = ups_find_config_reg(mgr->ups, reg_name);
            if (reg && reg->writable) {
                if (ups_config_write(mgr->ups, reg, (uint16_t)raw_val) == 0) {
                    log_info("UPS register write accepted");
                    result_append(mgr, res, "phase2", "ups", 0, NULL);
                } else {
                    log_error("UPS register write FAILED");
                    result_append(mgr, res, "phase2", "ups", 1, cutils_get_error());
                }
            } else {
                log_error("UPS register '%s' not found or not writable", reg_name);
                result_append(mgr, res, "phase2", "ups", 1,
                              "register not found or not writable");
            }
        } else {
            log_error("UPS action mode is 'register' but no register name configured");
            result_append(mgr, res, "phase2", "ups", 1,
                          "shutdown.ups_register is empty");
        }
    } else {
        log_warn("unknown UPS action mode: '%s'", ups_mode);
        result_append(mgr, res, "phase2", "ups", 1, "unknown shutdown.ups_mode");
    }

    /* Phase 2 summary event. */
    {
        size_t phase2_failed = res ? res->n_failed - phase2_failed_at_0 : 0;
        char buf[160];
        if (strcmp(ups_mode, "none") == 0) {
            snprintf(buf, sizeof(buf), "Skipped (mode=none)");
        } else if (dry_run) {
            snprintf(buf, sizeof(buf),
                     "%s (dry-run, mode=%s)",
                     phase2_failed ? "Pre-flight FAILED" : "Pre-flight ok",
                     ups_mode);
        } else {
            snprintf(buf, sizeof(buf),
                     "%s (mode=%s)",
                     phase2_failed ? "FAILED" : "Command accepted",
                     ups_mode);
        }
        emit_event(mgr, phase2_failed ? "error" : "info", "UPS Action", buf);
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
    /* Phase 3 also drives the terminal event for the workflow. The exact
     * shape depends on the branch we take:
     *   - dry-run / docker / controller_enabled=0 → daemon survives, we
     *     emit "Shutdown Workflow Complete" or "Dry Run Complete" with
     *     the elapsed time
     *   - real + controller_enabled=1 + poweroff accepted → daemon is
     *     about to be SIGTERM'd by systemd; "Controller Poweroff Initiated"
     *     is the last event we trust to land
     *   - real + controller_enabled=1 + poweroff failed → daemon survives,
     *     we emit "Controller Poweroff Failed" plus a final "Workflow
     *     Complete" with the failure noted */
    int real_poweroff_initiated = 0;
    int real_poweroff_failed    = 0;

    if (is_docker_build) {
        log_info("Controller: skipped (docker build cannot power off host)");
        result_append(mgr, res, "phase3", "controller", 2, NULL);
    } else if (ctrl_enabled) {
        if (dry_run) {
            /* Ask logind directly via D-Bus whether the calling user can
             * power off without an interactive challenge. Returns one of
             * "yes" (our polkit rule is in place), "no" (denied), or
             * "challenge" (auth needed — i.e. polkit rule missing for
             * this user). systemctl has no `can-poweroff` verb; the
             * canonical check is org.freedesktop.login1.Manager.CanPowerOff. */
            int can_rc = run_shell(
                "busctl call org.freedesktop.login1 /org/freedesktop/login1 "
                "org.freedesktop.login1.Manager CanPowerOff 2>/dev/null "
                "| grep -q '\"yes\"'");
            if (can_rc == 0) {
                log_info("Controller: logind would authorize poweroff");
                result_append(mgr, res, "phase3", "controller", 0, NULL);
            } else {
                log_error("Controller: logind refused poweroff (polkit grant missing?)");
                result_append(mgr, res, "phase3", "controller", 1,
                              "logind refused poweroff (polkit grant missing?)");
            }
        } else {
            log_info("Controller: shutting down via systemctl poweroff");
            /* Emit BEFORE the syscall: once systemctl returns, we're
             * racing systemd's SIGTERM and any later DB writes may not
             * survive. monitor_fire_event's INSERT is synchronous, so
             * by the time this returns the row is queued in the SQLite
             * journal — surviving the kill in all but the rarest cases. */
            emit_event(mgr, "critical", "Controller Poweroff Initiated",
                       "systemctl poweroff handed off to systemd");

            /* Goes through logind → systemd, gracefully stops services and
             * syncs filesystems. The .deb ships a polkit rule granting the
             * airies-ups system user org.freedesktop.login1.power-off; no
             * sudo, no setuid binary, no root. */
            int shut_rc = run_shell("systemctl poweroff");
            if (shut_rc != 0) {
                log_warn("controller shutdown command returned %d", shut_rc);
                char buf[64];
                snprintf(buf, sizeof(buf), "systemctl poweroff exited %d", shut_rc);
                result_append(mgr, res, "phase3", "controller", 1, buf);
                emit_event(mgr, "error", "Controller Poweroff Failed", buf);
                real_poweroff_failed = 1;
            } else {
                result_append(mgr, res, "phase3", "controller", 0, NULL);
                real_poweroff_initiated = 1;
            }
        }
    } else {
        log_info("Controller: shutdown disabled (skipped)");
        result_append(mgr, res, "phase3", "controller", 2, NULL);
    }

    char msg[128];
    long elapsed = (long)(time(NULL) - start);
    snprintf(msg, sizeof(msg), "=== SHUTDOWN WORKFLOW COMPLETE in %lds ===", elapsed);
    log_info("%s", msg);

    /* Terminal "complete" event — emit only when we have a future to live
     * for. After "Controller Poweroff Initiated" the daemon is about to
     * die, and an additional event is both noise and a write race with
     * systemd's SIGTERM. */
    if (!real_poweroff_initiated) {
        size_t total_failed = res ? res->n_failed : 0;
        char fbuf[160];
        snprintf(fbuf, sizeof(fbuf), "Elapsed %lds, %zu failure%s",
                 elapsed, total_failed, total_failed == 1 ? "" : "s");
        const char *title = dry_run ? "Dry Run Complete"
                                    : (real_poweroff_failed
                                          ? "Shutdown Workflow Halted"
                                          : "Shutdown Workflow Complete");
        const char *sev = (total_failed > 0 || real_poweroff_failed)
                              ? "error" : "info";
        emit_event(mgr, sev, title, fbuf);
    }

    if (out) {
        *out = res;
    } else {
        shutdown_result_free(res);
    }
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
        if (mgr->trigger_first_met != 0) {
            char buf[160];
            snprintf(buf, sizeof(buf),
                     "Shutdown trigger condition cleared before %ds debounce elapsed",
                     delay_sec);
            log_info("%s", buf);
            emit_event(mgr, "info", "Shutdown Trigger Cleared", buf);
        }
        mgr->trigger_first_met = 0;
        return;
    }

    /* Debounce */
    time_t now = time(NULL);
    if (mgr->trigger_first_met == 0) {
        mgr->trigger_first_met = now;
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "Shutdown trigger condition met, debouncing %ds", delay_sec);
        log_warn("%s", buf);
        emit_event(mgr, "warning", "Shutdown Trigger Armed", buf);
        return;
    }

    if (now - mgr->trigger_first_met < delay_sec)
        return;

    /* Fire — kick the workflow off on the worker thread so the monitor
     * poll loop (which calls us) keeps running. The auto-trigger path
     * is fire-and-forget; status flows through the same get_status
     * surface the API uses. */
    log_error("SHUTDOWN TRIGGER: conditions held for %lds — executing workflow",
              now - mgr->trigger_first_met);
    mgr->triggered = 1;
    int rc = shutdown_start_async(mgr, 0, NULL);
    if (rc != CUTILS_OK && rc != -EALREADY) {
        log_error("auto-trigger: failed to start workflow: %s",
                  cutils_get_error());
    }
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

int shutdown_test_target_confirm(shutdown_mgr_t *mgr, const char *target_name)
{
    const char *params[] = { target_name, NULL };
    CUTILS_AUTO_DBRES db_result_t *result = NULL;

    int rc = db_execute(mgr->db,
        "SELECT confirm_method, host, confirm_port, confirm_command "
        "FROM shutdown_targets WHERE name = ?",
        params, &result);

    if (rc != CUTILS_OK || !result || result->nrows == 0)
        return set_error(CUTILS_ERR_NOT_FOUND, "target '%s' not found", target_name);

    const char *method = result->rows[0][0];
    const char *host   = result->rows[0][1];
    int port = result->rows[0][2] ? atoi(result->rows[0][2]) : 0;
    const char *cmd    = result->rows[0][3];

    log_info("testing target '%s' down-detect (%s on %s)",
             target_name, method, host ? host : "?");
    rc = test_target_confirm(method, host, port, cmd);

    if (rc == 0)
        log_info("target '%s': down-detect OK (host reachable)", target_name);
    else
        log_error("target '%s': down-detect FAILED (host not reachable via %s)",
                  target_name, method ? method : "?");

    return rc;
}

/* --- Async workflow --- */

static void *worker_main(void *arg)
{
    shutdown_mgr_t *mgr = arg;

    int dry_run;
    pthread_mutex_lock(&mgr->state_lock);
    dry_run = mgr->active_dry_run;
    pthread_mutex_unlock(&mgr->state_lock);

    /* Run the workflow body. result_append (called from inside) mirrors
     * each step into mgr->snapshot under the state_lock so polled
     * readers see live progress. The local `res` here is redundant but
     * keeps shutdown_execute_ex's existing test-friendly out param
     * working — we just discard it. */
    shutdown_result_t *res = NULL;
    (void)shutdown_execute_ex(mgr, dry_run, &res);
    shutdown_result_free(res);

    pthread_mutex_lock(&mgr->state_lock);
    mgr->state       = SHUTDOWN_COMPLETED;
    mgr->finished_at = time(NULL);
    pthread_mutex_unlock(&mgr->state_lock);

    return NULL;
}

int shutdown_start_async(shutdown_mgr_t *mgr, int dry_run, uint64_t *out_id)
{
    if (!mgr) return set_error(CUTILS_ERR_INVALID, "shutdown_start_async: mgr is NULL");

    pthread_t prior;
    int       prior_valid;

    pthread_mutex_lock(&mgr->state_lock);

    if (mgr->state == SHUTDOWN_RUNNING) {
        if (out_id) *out_id = mgr->workflow_id;
        pthread_mutex_unlock(&mgr->state_lock);
        return -EALREADY;
    }

    /* Capture the previous run's worker handle so we can join it
     * outside the lock (pthread_join can be slow if the thread is
     * still wrapping up an emit_event DB write). */
    prior               = mgr->worker;
    prior_valid         = mgr->worker_started;
    mgr->worker_started = 0;

    /* Reset snapshot for the new run. The completed snapshot from a
     * prior workflow lingers until exactly this point — that's the
     * "until next start" retention contract from the API plan. */
    if (mgr->snapshot) {
        free(mgr->snapshot->steps);
        free(mgr->snapshot);
        mgr->snapshot = NULL;
    }
    mgr->snapshot = calloc(1, sizeof(*mgr->snapshot));
    /* If alloc fails the worker still runs; per-step mirroring drops
     * the row but the workflow itself proceeds — same trade-off as
     * the original res allocation. */

    mgr->state              = SHUTDOWN_RUNNING;
    mgr->active_dry_run     = dry_run ? 1 : 0;
    mgr->workflow_id       += 1;
    mgr->started_at         = time(NULL);
    mgr->finished_at        = 0;
    mgr->current_phase[0]   = '\0';
    mgr->current_target[0]  = '\0';
    if (out_id) *out_id     = mgr->workflow_id;

    pthread_mutex_unlock(&mgr->state_lock);

    if (prior_valid) pthread_join(prior, NULL);

    pthread_t newt;
    int rc = pthread_create(&newt, NULL, worker_main, mgr);
    if (rc != 0) {
        pthread_mutex_lock(&mgr->state_lock);
        mgr->state       = SHUTDOWN_IDLE;
        mgr->finished_at = time(NULL);
        pthread_mutex_unlock(&mgr->state_lock);
        return set_error(CUTILS_ERR, "failed to spawn shutdown worker: %s",
                         strerror(rc));
    }

    pthread_mutex_lock(&mgr->state_lock);
    mgr->worker         = newt;
    mgr->worker_started = 1;
    pthread_mutex_unlock(&mgr->state_lock);

    return CUTILS_OK;
}

void shutdown_get_status(shutdown_mgr_t *mgr, shutdown_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!mgr) return;

    pthread_mutex_lock(&mgr->state_lock);

    out->state       = mgr->state;
    out->workflow_id = mgr->workflow_id;
    out->dry_run     = mgr->active_dry_run;
    out->started_at  = mgr->started_at;
    out->finished_at = mgr->finished_at;
    snprintf(out->current_phase,  sizeof(out->current_phase),
             "%s", mgr->current_phase);
    snprintf(out->current_target, sizeof(out->current_target),
             "%s", mgr->current_target);

    if (mgr->snapshot && mgr->snapshot->n_steps > 0) {
        out->n_steps  = mgr->snapshot->n_steps;
        out->n_failed = mgr->snapshot->n_failed;
        out->steps    = malloc(out->n_steps * sizeof(*out->steps));
        if (out->steps) {
            memcpy(out->steps, mgr->snapshot->steps,
                   out->n_steps * sizeof(*out->steps));
        } else {
            /* Allocation failure: drop the array but keep the
             * scalar counters so the caller can at least render
             * "N steps, M failed". */
            out->n_steps = 0;
        }
    }

    out->all_ok = (mgr->state == SHUTDOWN_COMPLETED && out->n_failed == 0)
                  ? 1 : 0;

    pthread_mutex_unlock(&mgr->state_lock);
}

void shutdown_status_free(shutdown_status_t *out)
{
    if (!out) return;
    free(out->steps);
    out->steps   = NULL;
    out->n_steps = 0;
}
