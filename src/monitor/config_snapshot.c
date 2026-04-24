#include "monitor/config_snapshot.h"

#include <stdlib.h>

config_snapshot_decision_t
monitor_config_snapshot_decide(cutils_db_t *db, const char *register_name,
                               uint16_t current_raw)
{
    const char *params[] = { register_name, NULL };
    db_result_t *res = NULL;
    int rc = db_execute(db,
        "SELECT raw_value FROM ups_config WHERE register_name = ? "
        "ORDER BY id DESC LIMIT 1",
        params, &res);

    if (rc != 0 || !res || res->nrows == 0) {
        db_result_free(res);
        return CONFIG_SNAPSHOT_BASELINE;
    }
    long prev = strtol(res->rows[0][0], NULL, 10);
    db_result_free(res);

    if (prev == (long)current_raw) return CONFIG_SNAPSHOT_SKIP;
    return CONFIG_SNAPSHOT_EXTERNAL;
}
