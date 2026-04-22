#ifndef MONITOR_STATUS_SNAPSHOT_H
#define MONITOR_STATUS_SNAPSHOT_H

#include <stdint.h>
#include <cutils/db.h>

/* Last-known UPS status persisted to SQLite, updated on every status
 * change. The monitor and alert engine load this at daemon startup so
 * transition detection works against reality rather than a zeroed
 * in-memory state — without this, any fault that was already active
 * before the daemon started would never produce an event because the
 * very-first poll's `prev` and `current` look identical.
 *
 * The snapshot deliberately holds only the fields that are diffed for
 * transition detection (the bitfields in ups_data_t, not the per-poll
 * measurements). Backing table is `ups_status_snapshot` (migration 011).
 *
 * Failure modes:
 *   - Power loss between observation and commit: SQLite transaction
 *     atomicity guarantees we never see partial state on disk; we lose
 *     at most one status-change observation (~10ms window).
 *   - DB wipe / fresh install: load() returns -1 and the caller falls
 *     back to a baseline (typically UPS_ST_ONLINE only) — first poll
 *     fires events for every non-baseline condition, which matches the
 *     "fresh discovery" UX. */

typedef struct {
    uint32_t status;              /* UPS_ST_* bits */
    uint16_t bat_system_error;    /* UPS_BATERR_* bits */
    uint16_t general_error;       /* UPS_GENERR_* bits */
    uint32_t power_system_error;  /* UPS_PWRERR_* bits */
    uint16_t bat_lifetime_status; /* APC Battery.LifeTimeStatus_BF raw */
    char     updated_at[32];      /* ISO-8601 UTC, set by status_snapshot_save */
} status_snapshot_t;

/* Load the persisted snapshot. Returns 0 on success (row exists, *out
 * fully populated) or -1 if no row exists yet or on DB error. Caller
 * uses the -1 path to apply a baseline default. */
int status_snapshot_load(cutils_db_t *db, status_snapshot_t *out);

/* UPSERT the singleton row with the given snapshot. updated_at is
 * filled by this function with the current UTC time. Returns 0 on
 * success, non-zero on DB error.
 *
 * Callers should invoke this only when the diff fields actually
 * changed — there's no value in writing identical state every poll
 * (and "write only on change" is the property the failure-mode
 * analysis depends on). */
int status_snapshot_save(cutils_db_t *db, status_snapshot_t *snap);

#endif
