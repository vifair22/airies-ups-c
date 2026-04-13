#ifndef ALERTS_H
#define ALERTS_H

#include "ups.h"
#include "config.h"

/* Transfer voltage thresholds read from UPS registers */
typedef struct {
    uint16_t transfer_high;  /* reg 1026 */
    uint16_t transfer_low;   /* reg 1027 */
} ups_thresholds_t;

/* Hysteresis alert state — tracks which alerts are currently active */
typedef struct {
    int overload;       /* UPS_ST_OVERLOAD */
    int fault;          /* UPS_ST_FAULT */
    int bat_replace;    /* UPS_BATERR_REPLACE */
    int input_high;     /* input_voltage approaching transfer_high */
    int input_low;      /* input_voltage approaching transfer_low */
    int load_high;      /* load_pct > config threshold */
    int bat_low;        /* charge_pct < config threshold, gated */
    double prev_charge; /* for battery low recovery gating */
} alert_state_t;

/* Callback for sending alert notifications */
typedef void (*alert_notify_fn)(const char *title, const char *body);

/* Initialize alert state (all clear) */
void alerts_init(alert_state_t *state);

/*
 * Check current UPS data against alert thresholds.
 *
 * Fires notifications via notify_fn on state transitions (entering/leaving
 * alert conditions). Returns a bitmask of UPS_ST_* bits that had dedicated
 * alerts fire this cycle, so the caller can suppress generic "Status Change"
 * notifications for those bits.
 *
 * thresh may be NULL if thresholds weren't read (disables voltage alerts).
 */
uint32_t alerts_check(alert_state_t *state,
                      const ups_data_t *data,
                      const ups_thresholds_t *thresh,
                      const config_t *cfg,
                      alert_notify_fn notify);

#endif
