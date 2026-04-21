#ifndef RETENTION_H
#define RETENTION_H

#include <cutils/db.h>

/* --- Telemetry Retention ---
 *
 * Configurable retention policy:
 *   - Full resolution for N hours (default 24)
 *   - Downsample older data to M-minute averages (default 15)
 *   - Delete data older than D days (default 90)
 *
 * Runs daily as part of the monitor lifecycle. */

typedef struct {
    int full_res_hours;    /* keep full resolution for this many hours */
    int downsample_minutes; /* downsample older data to this interval */
    int retention_days;    /* delete data older than this */
} retention_config_t;

/* Run retention cleanup: downsample + delete old data + vacuum.
 * Safe to call from any thread (uses DB mutex). */
int retention_run(cutils_db_t *db, const retention_config_t *cfg);

#endif
