#ifndef MONITOR_CONFIG_SNAPSHOT_H
#define MONITOR_CONFIG_SNAPSHOT_H

#include <stdint.h>
#include <cutils/db.h>

/* Decision made by the daily config snapshot pass for a single register.
 * Keeps the diff logic isolated from the driver- and monitor-heavy file
 * it's called from, so it can be unit tested with just a DB fixture. */
typedef enum {
    CONFIG_SNAPSHOT_SKIP     = 0,  /* Same as last recorded value — no insert */
    CONFIG_SNAPSHOT_BASELINE = 1,  /* No prior row for this register */
    CONFIG_SNAPSHOT_EXTERNAL = 2,  /* Differs from last row — changed outside our API */
} config_snapshot_decision_t;

config_snapshot_decision_t
monitor_config_snapshot_decide(cutils_db_t *db,
                               const char *register_name,
                               uint32_t current_raw);

#endif
