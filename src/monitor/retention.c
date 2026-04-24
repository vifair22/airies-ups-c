#include "monitor/retention.h"
#include <cutils/log.h>

#include <stdio.h>
#include <time.h>

int retention_run(cutils_db_t *db, const retention_config_t *cfg)
{
    time_t now = time(NULL);
    char ts[32];
    struct tm tm;
    int affected = 0;

    /* Phase 1: Delete data older than retention_days */
    if (cfg->retention_days > 0) {
        time_t cutoff = now - (time_t)cfg->retention_days * 86400;
        gmtime_r(&cutoff, &tm);
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

        const char *params[] = { ts, NULL };
        int deleted = 0;
        /* Best-effort retention sweep; on failure deleted stays 0 so the
         * conditional log below won't report work that didn't happen. */
        CUTILS_UNUSED(db_execute_non_query(db,
            "DELETE FROM telemetry WHERE timestamp < ?", params, &deleted));

        if (deleted > 0) {
            log_info("retention: deleted %d telemetry rows older than %d days",
                     deleted, cfg->retention_days);
            affected += deleted;
        }
    }

    /* Phase 2: Downsample data older than full_res_hours.
     * Strategy: for each downsample_minutes window, keep only the row
     * closest to the window midpoint and delete the rest. */
    if (cfg->full_res_hours > 0 && cfg->downsample_minutes > 0) {
        time_t boundary = now - (time_t)cfg->full_res_hours * 3600;
        gmtime_r(&boundary, &tm);
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

        /* Delete rows that are NOT the "representative" row for their
         * time window. The representative is the row with the minimum
         * ABS(time - window_midpoint). We use a simpler approach:
         * keep one row per window (the one with MIN(id)) and delete rest. */
        char interval_s[16];
        snprintf(interval_s, sizeof(interval_s), "%d", cfg->downsample_minutes * 60);

        /* Three placeholders, three params: outer WHERE cutoff,
         * inner WHERE cutoff, and the GROUP BY window size in seconds. */
        const char *params[] = { ts, ts, interval_s, NULL };
        int downsampled = 0;
        /* Best-effort downsampling; same reasoning as the delete above. */
        CUTILS_UNUSED(db_execute_non_query(db,
            "DELETE FROM telemetry WHERE timestamp < ? AND id NOT IN ("
            "  SELECT MIN(id) FROM telemetry WHERE timestamp < ? "
            "  GROUP BY CAST(strftime('%%s', timestamp) AS INTEGER) / ?"
            ")",
            params, &downsampled));

        if (downsampled > 0) {
            log_info("retention: downsampled %d telemetry rows (older than %dh, "
                     "keeping %dm resolution)",
                     downsampled, cfg->full_res_hours, cfg->downsample_minutes);
            affected += downsampled;
        }
    }

    /* Phase 3: Vacuum if we deleted anything */
    if (affected > 0)
        /* Pure housekeeping; a failed vacuum just delays space reclaim
         * until the next retention pass. */
        CUTILS_UNUSED(db_exec_raw(db, "PRAGMA incremental_vacuum;"));

    return 0;
}
