#ifndef SHUTDOWN_H
#define SHUTDOWN_H

#include "ups/ups.h"
#include "monitor/monitor.h"
#include <cutils/db.h>
#include <cutils/config.h>

#include <stdint.h>
#include <time.h>

/* --- Shutdown Orchestrator ---
 *
 * DB-backed shutdown targets and groups.
 * Groups execute sequentially (by execution_order).
 * Targets within a group execute in parallel or sequentially.
 *
 * After all groups: configurable UPS action, then controller shutdown.
 * Both final phases are driven by app config keys (shutdown.* namespace). */

/* Shutdown handle */
typedef struct shutdown_mgr shutdown_mgr_t;

/* Progress callback — called for each target as it starts/completes */
typedef void (*shutdown_progress_fn)(const char *group, const char *target,
                                     const char *status, void *userdata);

/* One row per executed/skipped step in a workflow run. The same struct
 * is used for real and dry-run executions; for dry-run, ok=0 means
 * pre-flight validation passed, ok=1 means it failed.
 *
 * Phase identifiers:
 *   "phase1"    user-defined group target (Phase 1)
 *   "phase2"    UPS shutdown action       (Phase 2)
 *   "phase3"    controller poweroff       (Phase 3)
 *
 * `target` carries:
 *   "<group>/<target>"   for Phase 1
 *   "ups"                for Phase 2
 *   "controller"         for Phase 3 */
typedef struct {
    char phase[16];
    char target[96];
    int  ok;            /* 0 = ok, 1 = failed, 2 = skipped */
    char error[256];    /* short reason on failure (empty on ok/skipped) */
} shutdown_step_result_t;

/* Aggregated result of a workflow run. Allocated by shutdown_execute_ex,
 * freed via shutdown_result_free. */
typedef struct {
    shutdown_step_result_t *steps;
    size_t                  n_steps;
    size_t                  n_failed;
} shutdown_result_t;

void shutdown_result_free(shutdown_result_t *res);

/* Create the shutdown manager.
 *
 * `mon` is the optional event sink: when non-NULL, workflow milestones
 * (trigger arm/clear, workflow start, per-phase outcome, terminal event)
 * are mirrored into the events table via monitor_fire_event so the UI's
 * Events page records them alongside battery/power events. Per-target
 * detail stays in the daemon log. Pass NULL in unit tests or any context
 * where the monitor isn't wired — event emission no-ops cleanly. */
shutdown_mgr_t *shutdown_create(cutils_db_t *db, ups_t *ups,
                                cutils_config_t *config, monitor_t *mon);

/* Free the shutdown manager. */
void shutdown_free(shutdown_mgr_t *mgr);

/* Register a progress callback. */
void shutdown_on_progress(shutdown_mgr_t *mgr, shutdown_progress_fn fn,
                          void *userdata);

/* Execute the full shutdown workflow.
 * If dry_run is non-zero, logs what would happen without executing.
 * UPS action mode and controller shutdown are driven by app config.
 * Returns 0 on success. */
int shutdown_execute(shutdown_mgr_t *mgr, int dry_run);

/* Variant of shutdown_execute that captures per-step results for the API
 * to surface back to the caller. If `out` is non-NULL, *out is set to a
 * heap-allocated result the caller must free with shutdown_result_free.
 * Pass NULL to behave identically to shutdown_execute. */
int shutdown_execute_ex(shutdown_mgr_t *mgr, int dry_run,
                        shutdown_result_t **out);

/* Test a single target by name (connect + verify, don't shut down).
 * Returns 0 on success. */
int shutdown_test_target(shutdown_mgr_t *mgr, const char *target_name);

/* Test that the target's configured down-detect method can currently
 * observe the host as up — i.e., a real shutdown's wait-for-down loop
 * would have something valid to watch. method="none" is trivially ok.
 * Returns 0 if reachable, set_error()'d non-zero otherwise. */
int shutdown_test_target_confirm(shutdown_mgr_t *mgr, const char *target_name);

/* Evaluate trigger conditions against current UPS data.
 * Called from the monitor poll loop. If conditions are met for the
 * configured debounce period, executes the shutdown workflow.
 * Safe to call frequently — internally tracks state and debounce. */
void shutdown_check_trigger(shutdown_mgr_t *mgr, const ups_data_t *data);

/* --- Async workflow ---
 *
 * shutdown_start_async fires the workflow on a dedicated worker thread
 * and returns immediately so the caller (API request thread, monitor
 * poll loop) is never blocked by the multi-minute group/UPS/controller
 * sequence. Only one workflow runs at a time per manager.
 *
 * Status flows IDLE -> RUNNING -> COMPLETED. Completed snapshot lingers
 * until the next start_async resets it. Test paths that want
 * synchronous behavior continue to use shutdown_execute(_ex). */

typedef enum {
    SHUTDOWN_IDLE      = 0,
    SHUTDOWN_RUNNING   = 1,
    SHUTDOWN_COMPLETED = 2,
} shutdown_state_t;

/* Snapshot of an in-flight or completed workflow run. shutdown_get_status
 * deep-copies into the caller's `out` (including a fresh `steps` array)
 * so the caller can drop the manager lock before serializing. Free with
 * shutdown_status_free. */
typedef struct {
    shutdown_state_t        state;
    uint64_t                workflow_id;     /* monotonic; 0 before any start */
    int                     dry_run;         /* meaningful when state != IDLE */
    time_t                  started_at;
    time_t                  finished_at;     /* 0 while RUNNING */
    char                    current_phase[16];
    char                    current_target[96];
    size_t                  n_steps;
    size_t                  n_failed;
    int                     all_ok;          /* 1 only when COMPLETED && n_failed == 0 */
    shutdown_step_result_t *steps;           /* heap-owned snapshot, NULL if n_steps == 0 */
} shutdown_status_t;

/* Start the workflow on the manager's worker thread. Returns CUTILS_OK
 * on accepted; a new workflow_id is written to *out_id (if non-NULL).
 * Returns -EALREADY if a workflow is already RUNNING — *out_id is set
 * to the in-flight workflow's id so the caller can attach to it. */
int  shutdown_start_async(shutdown_mgr_t *mgr, int dry_run, uint64_t *out_id);

/* Snapshot the current workflow state under the manager lock and write
 * a deep copy into *out. Always succeeds for a valid mgr; out->state
 * == SHUTDOWN_IDLE before any start_async has been called. Caller frees
 * out->steps via shutdown_status_free. */
void shutdown_get_status(shutdown_mgr_t *mgr, shutdown_status_t *out);

/* Release the heap-owned fields of a status snapshot. Safe on a zeroed
 * struct. Does not free `out` itself. */
void shutdown_status_free(shutdown_status_t *out);

#endif
