#include "alerts/alerts.h"
#include <cutils/config.h>
#include <cutils/log.h>

#include <stdio.h>
#include <string.h>

void alerts_init(alert_state_t *state)
{
    memset(state, 0, sizeof(*state));
    state->prev_charge = -1.0;
}

alert_config_t alerts_load_config(const void *cfg_ptr)
{
    const cutils_config_t *cfg = cfg_ptr;
    alert_config_t acfg;
    acfg.load_high_pct       = config_get_int(cfg, "alerts.load_high_pct", 80);
    acfg.battery_low_pct     = config_get_int(cfg, "alerts.battery_low_pct", 50);
    acfg.voltage_warn_offset = config_get_int(cfg, "alerts.voltage_warn_offset", 5);
    acfg.voltage_deadband    = config_get_int(cfg, "alerts.voltage_deadband", 1);
    return acfg;
}

/* --- Helpers --- */

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

static void check_threshold_alert(int *prev, int current,
                                  const char *title_on, const char *title_off,
                                  const char *body, alert_notify_fn notify)
{
    if (current && !*prev)
        notify(title_on, body);
    else if (!current && *prev)
        notify(title_off, body);

    *prev = current;
}

/* --- Main check --- */

uint32_t alerts_check(alert_state_t *state,
                      const ups_data_t *data,
                      const alert_thresholds_t *thresh,
                      const alert_config_t *acfg,
                      alert_notify_fn notify)
{
    uint32_t alerted = 0;
    char body[512];

    /* Overload */
    int is_overload = (data->status & UPS_ST_OVERLOAD) != 0;
    snprintf(body, sizeof(body), "Load: %.0f%%", data->load_pct);
    alerted |= check_bit_alert(&state->overload, is_overload,
                                "UPS Overload", "UPS Overload Cleared",
                                body, UPS_ST_OVERLOAD, notify);

    /* Fault */
    int is_fault = (data->status & UPS_ST_FAULT) != 0;
    {
        char status_str[256];
        ups_status_str(data->status, status_str, sizeof(status_str));
        snprintf(body, sizeof(body), "Status: %s", status_str);
    }
    alerted |= check_bit_alert(&state->fault, is_fault,
                                "UPS Fault", "UPS Fault Cleared",
                                body, UPS_ST_FAULT, notify);

    /* Battery replace (reg 22, bit 2) */
    int is_bat_replace = (data->bat_system_error & UPS_BATERR_REPLACE) != 0;
    snprintf(body, sizeof(body), "Battery system error detected");
    if (is_bat_replace && !state->bat_replace)
        notify("UPS Battery Replace Required", body);
    else if (!is_bat_replace && state->bat_replace)
        notify("UPS Battery Error Cleared", body);
    state->bat_replace = is_bat_replace;

    /* Input voltage high */
    if (thresh && thresh->transfer_high > 0) {
        double high_enter = thresh->transfer_high - acfg->voltage_warn_offset;
        double high_clear = high_enter - acfg->voltage_deadband;
        int is_input_high = state->input_high
            ? (data->input_voltage > high_clear)
            : (data->input_voltage > high_enter);
        snprintf(body, sizeof(body), "Input: %.1fV (transfer at %uV, warn at %.0fV)",
                 data->input_voltage, thresh->transfer_high, high_enter);
        check_threshold_alert(&state->input_high, is_input_high,
                              "UPS Input Voltage High", "UPS Input Voltage Normal",
                              body, notify);
    }

    /* Input voltage low */
    if (thresh && thresh->transfer_low > 0) {
        double low_enter = thresh->transfer_low + acfg->voltage_warn_offset;
        double low_clear = low_enter + acfg->voltage_deadband;
        int is_input_low = state->input_low
            ? (data->input_voltage < low_clear)
            : (data->input_voltage < low_enter);
        snprintf(body, sizeof(body), "Input: %.1fV (transfer at %uV, warn at %.0fV)",
                 data->input_voltage, thresh->transfer_low, low_enter);
        check_threshold_alert(&state->input_low, is_input_low,
                              "UPS Input Voltage Low", "UPS Input Voltage Normal",
                              body, notify);
    }

    /* Load high */
    {
        int is_load_high = data->load_pct > acfg->load_high_pct;
        snprintf(body, sizeof(body), "Load: %.0f%% (threshold: %d%%)",
                 data->load_pct, acfg->load_high_pct);
        check_threshold_alert(&state->load_high, is_load_high,
                              "UPS Load High", "UPS Load Normal",
                              body, notify);
    }

    /* Battery low (gated: online + charge not rising) */
    {
        int is_online = (data->status & UPS_ST_ONLINE) != 0;
        int charge_below = data->charge_pct < acfg->battery_low_pct;
        int charge_not_rising = (state->prev_charge >= 0.0) &&
                                (data->charge_pct <= state->prev_charge);

        int is_bat_low = charge_below && is_online && charge_not_rising;
        int was_bat_low = state->bat_low;
        int recovered = was_bat_low &&
                        (!charge_below || !is_online ||
                         data->charge_pct > state->prev_charge);

        if (is_bat_low && !was_bat_low) {
            snprintf(body, sizeof(body), "Battery: %.0f%% (threshold: %d%%)",
                     data->charge_pct, acfg->battery_low_pct);
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
