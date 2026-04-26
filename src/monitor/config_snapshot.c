#include "monitor/config_snapshot.h"
#include <cutils/db.h>

#include <stdlib.h>

config_snapshot_decision_t
monitor_config_snapshot_decide(cutils_db_t *db, const char *register_name,
                               uint32_t current_raw)
{
    const char *params[] = { register_name, NULL };
    CUTILS_AUTO_DBRES db_result_t *res = NULL;
    int rc = db_execute(db,
        "SELECT raw_value FROM ups_config WHERE register_name = ? "
        "ORDER BY id DESC LIMIT 1",
        params, &res);

    if (rc != 0 || !res || res->nrows == 0)
        return CONFIG_SNAPSHOT_BASELINE;

    unsigned long prev = strtoul(res->rows[0][0], NULL, 10);
    if (prev == (unsigned long)current_raw) return CONFIG_SNAPSHOT_SKIP;
    return CONFIG_SNAPSHOT_EXTERNAL;
}
