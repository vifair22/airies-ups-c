#include "alerts.h"
#include <stdio.h>
#include <string.h>

void alerts_init(alert_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->prev_charge = -1.0; /* sentinel: no previous reading */
}

/*
 * Helper: check a discrete bit alert. On transition, send notification.
 * Returns the bit value if a dedicated alert fired (for suppression mask).
 */
static uint32_t check_bit_alert(int *prev, int current,
                                const char *title_on, const char *title_off,
                                const char *body, uint32_t bit,
                                alert_notify_fn notify)
{
    uint32_t alerted = 0;

    if (current && !*prev) {
        notify(title_on, body);
        alerted = bit;
    } else if (!current && *prev) {
        notify(title_off, body);
        alerted = bit;
    }

    *prev = current;
    return alerted;
}

/*
 * Helper: check a threshold alert with hysteresis.
 * Only fires on transitions (entering/leaving the alert condition).
 */
static void check_threshold_alert(int *prev, int current,
                                  const char *title_on, const char *title_off,
                                  const char *body,
                                  alert_notify_fn notify)
{
    if (current && !*prev)
        notify(title_on, body);
    else if (!current && *prev)
        notify(title_off, body);

    *prev = current;
}

uint32_t alerts_check(alert_state_t *state,
                      const ups_data_t *data,
                      const ups_thresholds_t *thresh,
                      const config_t *cfg,
                      alert_notify_fn notify)
{
    uint32_t alerted = 0;
    char body[512];

    /* --- Critical: Overload --- */
    int is_overload = (data->status & UPS_ST_OVERLOAD) != 0;
    snprintf(body, sizeof(body), "Load: %.0f%%", data->load_pct);
    alerted |= check_bit_alert(&state->overload, is_overload,
                                "UPS Overload", "UPS Overload Cleared",
                                body, UPS_ST_OVERLOAD, notify);

    /* --- Critical: Fault --- */
    int is_fault = (data->status & UPS_ST_FAULT) != 0;
    {
        char status_str[256];
        ups_status_str(data->status, status_str, sizeof(status_str));
        snprintf(body, sizeof(body), "Status: %s", status_str);
    }
    alerted |= check_bit_alert(&state->fault, is_fault,
                                "UPS Fault", "UPS Fault Cleared",
                                body, UPS_ST_FAULT, notify);

    /* --- Critical: Battery replace --- */
    int is_bat_replace = (data->bat_system_error & UPS_BATERR_REPLACE) != 0;
    snprintf(body, sizeof(body), "Battery system error detected");
    /* bat_replace uses BatterySystemError register (reg 22), not the
     * status register. No bit in alerted mask — bat_system_error changes
     * don't affect status_signature directly. */
    if (is_bat_replace && !state->bat_replace) {
        notify("UPS Battery Replace Required", body);
    } else if (!is_bat_replace && state->bat_replace) {
        notify("UPS Battery Error Cleared", body);
    }
    state->bat_replace = is_bat_replace;

    /* --- Warning: Input voltage high ---
     * Enter alert at (transfer_high - offset), clear at (transfer_high - offset - deadband).
     * Example: transfer=130, offset=5, deadband=1 → alert at 125V, clear at 124V */
    if (thresh && thresh->transfer_high > 0) {
        double high_enter = thresh->transfer_high - cfg->alert_voltage_warn_offset;
        double high_clear = high_enter - cfg->alert_voltage_deadband;
        int is_input_high = state->input_high
            ? (data->input_voltage > high_clear)   /* already alerting: hold until below clear */
            : (data->input_voltage > high_enter);   /* not alerting: enter at threshold */
        snprintf(body, sizeof(body), "Input: %.1fV (transfer at %uV, warn at %.0fV)",
                 data->input_voltage, thresh->transfer_high, high_enter);
        check_threshold_alert(&state->input_high, is_input_high,
                              "UPS Input Voltage High", "UPS Input Voltage Normal",
                              body, notify);
    }

    /* --- Warning: Input voltage low ---
     * Enter alert at (transfer_low + offset), clear at (transfer_low + offset + deadband).
     * Example: transfer=100, offset=5, deadband=1 → alert at 105V, clear at 106V */
    if (thresh && thresh->transfer_low > 0) {
        double low_enter = thresh->transfer_low + cfg->alert_voltage_warn_offset;
        double low_clear = low_enter + cfg->alert_voltage_deadband;
        int is_input_low = state->input_low
            ? (data->input_voltage < low_clear)    /* already alerting: hold until above clear */
            : (data->input_voltage < low_enter);    /* not alerting: enter at threshold */
        snprintf(body, sizeof(body), "Input: %.1fV (transfer at %uV, warn at %.0fV)",
                 data->input_voltage, thresh->transfer_low, low_enter);
        check_threshold_alert(&state->input_low, is_input_low,
                              "UPS Input Voltage Low", "UPS Input Voltage Normal",
                              body, notify);
    }

    /* --- Warning: Load high --- */
    {
        int is_load_high = data->load_pct > cfg->alert_load_high_pct;
        snprintf(body, sizeof(body), "Load: %.0f%% (threshold: %d%%)",
                 data->load_pct, cfg->alert_load_high_pct);
        check_threshold_alert(&state->load_high, is_load_high,
                              "UPS Load High", "UPS Load Normal",
                              body, notify);
    }

    /* --- Warning: Battery low ---
     * Gated: only fires when online AND charge is not increasing
     * (prevents false alerts during post-outage recharge) */
    {
        int is_online = (data->status & UPS_ST_ONLINE) != 0;
        int charge_below = data->charge_pct < cfg->alert_battery_low_pct;
        int charge_not_rising = (state->prev_charge >= 0.0) &&
                                (data->charge_pct <= state->prev_charge);

        int is_bat_low = charge_below && is_online && charge_not_rising;

        /* Recovery: charge rises above threshold OR UPS goes off-line */
        int was_bat_low = state->bat_low;
        int recovered = was_bat_low &&
                        (!charge_below || !is_online || data->charge_pct > state->prev_charge);

        if (is_bat_low && !was_bat_low) {
            snprintf(body, sizeof(body), "Battery: %.0f%% (threshold: %d%%)",
                     data->charge_pct, cfg->alert_battery_low_pct);
            notify("UPS Battery Low", body);
            state->bat_low = 1;
        } else if (recovered) {
            snprintf(body, sizeof(body), "Battery: %.0f%%", data->charge_pct);
            notify("UPS Battery Normal", body);
            state->bat_low = 0;
        }

        state->prev_charge = data->charge_pct;
    }

    return alerted;
}
