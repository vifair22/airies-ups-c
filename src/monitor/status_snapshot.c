#include "monitor/status_snapshot.h"
#include <cutils/log.h>

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int status_snapshot_load(cutils_db_t *db, status_snapshot_t *out)
{
    db_result_t *result = NULL;
    int rc = db_execute(db,
        "SELECT status, bat_system_error, general_error, "
        "       power_system_error, bat_lifetime_status, updated_at "
        "FROM ups_status_snapshot WHERE id = 1",
        NULL, &result);

    if (rc != 0 || !result || result->nrows == 0) {
        db_result_free(result);
        return -1;
    }

    /* All fields are stored as INTEGER (uint32 fits comfortably in
     * SQLite's INTEGER which is up to 64-bit signed). strtoul handles
     * the wider 32-bit values without sign-extension surprises. */
    char **row = result->rows[0];
    out->status              = (uint32_t)strtoul(row[0], NULL, 10);
    out->bat_system_error    = (uint16_t)strtoul(row[1], NULL, 10);
    out->general_error       = (uint16_t)strtoul(row[2], NULL, 10);
    out->power_system_error  = (uint32_t)strtoul(row[3], NULL, 10);
    out->bat_lifetime_status = (uint16_t)strtoul(row[4], NULL, 10);
    snprintf(out->updated_at, sizeof(out->updated_at), "%s",
             row[5] ? row[5] : "");

    db_result_free(result);
    return 0;
}

int status_snapshot_save(cutils_db_t *db, status_snapshot_t *snap)
{
    /* Stamp the snapshot with current UTC time before persistence. */
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(snap->updated_at, sizeof(snap->updated_at),
             "%Y-%m-%d %H:%M:%S", &tm);

    /* Stringify the integers — db_execute_non_query takes char* params. */
    char status_s[16], bat_err_s[8], gen_err_s[8], pwr_err_s[16], lifetime_s[8];
    snprintf(status_s,   sizeof(status_s),   "%u", snap->status);
    snprintf(bat_err_s,  sizeof(bat_err_s),  "%u", snap->bat_system_error);
    snprintf(gen_err_s,  sizeof(gen_err_s),  "%u", snap->general_error);
    snprintf(pwr_err_s,  sizeof(pwr_err_s),  "%u", snap->power_system_error);
    snprintf(lifetime_s, sizeof(lifetime_s), "%u", snap->bat_lifetime_status);

    /* SQLite UPSERT (3.24+, Pi runtime is comfortably newer). The
     * CHECK (id = 1) constraint in the schema enforces the singleton
     * shape; the ON CONFLICT clause handles the "row already exists"
     * branch without us needing a pre-check. */
    const char *params[] = {
        status_s, bat_err_s, gen_err_s, pwr_err_s, lifetime_s, snap->updated_at,
        NULL
    };
    int rc = db_execute_non_query(db,
        "INSERT INTO ups_status_snapshot "
        "  (id, status, bat_system_error, general_error, "
        "   power_system_error, bat_lifetime_status, updated_at) "
        "VALUES (1, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET "
        "  status              = excluded.status, "
        "  bat_system_error    = excluded.bat_system_error, "
        "  general_error       = excluded.general_error, "
        "  power_system_error  = excluded.power_system_error, "
        "  bat_lifetime_status = excluded.bat_lifetime_status, "
        "  updated_at          = excluded.updated_at",
        params, NULL);

    if (rc != 0)
        log_warn("status_snapshot_save: DB write failed");

    return rc;
}
