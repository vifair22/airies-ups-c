#ifndef SHUTDOWN_H
#define SHUTDOWN_H

#include "ups/ups.h"
#include <cutils/db.h>
#include <cutils/config.h>

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

/* Create the shutdown manager. */
shutdown_mgr_t *shutdown_create(cutils_db_t *db, ups_t *ups,
                                cutils_config_t *config);

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

/* Evaluate trigger conditions against current UPS data.
 * Called from the monitor poll loop. If conditions are met for the
 * configured debounce period, executes the shutdown workflow.
 * Safe to call frequently — internally tracks state and debounce. */
void shutdown_check_trigger(shutdown_mgr_t *mgr, const ups_data_t *data);

#endif
