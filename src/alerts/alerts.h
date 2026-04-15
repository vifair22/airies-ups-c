#ifndef ALERTS_H
#define ALERTS_H

#include "ups/ups.h"
#include <cutils/db.h>

/* --- Alert Engine ---
 *
 * Hysteresis-based alert system. Checks UPS data against thresholds
 * on each monitor poll. Fires events on state transitions only.
 *
 * Alert definitions are configured via app config (DB-backed).
 * Transfer voltage thresholds are read from the UPS registers. */

/* Alert state — tracks which alerts are currently active */
typedef struct {
    int    overload;
    int    fault;
    int    bat_replace;
    int    input_high;
    int    input_low;
    int    load_high;
    int    bat_low;
    double prev_charge;
} alert_state_t;

/* Transfer voltage thresholds (read from UPS) */
typedef struct {
    uint16_t transfer_high;
    uint16_t transfer_low;
} alert_thresholds_t;

/* Alert config (read from app config DB keys) */
typedef struct {
    int load_high_pct;
    int battery_low_pct;
    int voltage_warn_offset;
    int voltage_deadband;
} alert_config_t;

/* Callback for alert notifications */
typedef void (*alert_notify_fn)(const char *title, const char *body);

/* Initialize alert state (all clear). */
void alerts_init(alert_state_t *state);

/* Load alert config from c-utils config system. */
alert_config_t alerts_load_config(const void *cfg);

/* Run alert checks against current UPS data.
 * Fires notifications via notify_fn on state transitions.
 * Returns a bitmask of UPS_ST_* bits that had dedicated alerts fire,
 * so the caller can suppress generic status change notifications. */
uint32_t alerts_check(alert_state_t *state,
                      const ups_data_t *data,
                      const alert_thresholds_t *thresh,
                      const alert_config_t *acfg,
                      alert_notify_fn notify);

#endif
