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

/* Test a single target by name (connect + verify, don't shut down).
 * Returns 0 on success. */
int shutdown_test_target(shutdown_mgr_t *mgr, const char *target_name);

/* Evaluate trigger conditions against current UPS data.
 * Called from the monitor poll loop. If conditions are met for the
 * configured debounce period, executes the shutdown workflow.
 * Safe to call frequently — internally tracks state and debounce. */
void shutdown_check_trigger(shutdown_mgr_t *mgr, const ups_data_t *data);

#endif
