#include "ups.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- Driver registry --- */

extern const ups_driver_t ups_driver_srt;
extern const ups_driver_t ups_driver_smt;

const ups_driver_t *ups_drivers[] = {
    &ups_driver_srt,
    &ups_driver_smt,
    NULL,
};

/* --- Connection + auto-detect --- */

/* Read inventory for model string without a driver.
 * Uses the shared register layout (990-9840): reg 532, 16 regs = 32 chars. */
static int read_model_string(modbus_t *ctx, char *model, size_t model_sz)
{
    uint16_t regs[16];
    if (modbus_read_registers(ctx, 532, 16, regs) != 16)
        return -1;

    memset(model, 0, model_sz);
    size_t max_chars = model_sz - 1;
    if (max_chars > 32) max_chars = 32;
    for (size_t i = 0; i < max_chars / 2 && i < 16; i++) {
        model[i * 2]     = (char)((regs[i] >> 8) & 0xFF);
        model[i * 2 + 1] = (char)(regs[i] & 0xFF);
    }

    return 0;
}

static const ups_driver_t *detect_driver(const char *model)
{
    for (int i = 0; ups_drivers[i] != NULL; i++) {
        if (ups_drivers[i]->detect && ups_drivers[i]->detect(model))
            return ups_drivers[i];
    }
    return NULL;
}

ups_t *ups_connect(const char *device, int baud, int slave_id)
{
    modbus_t *ctx = modbus_new_rtu(device, baud, 'N', 8, 1);
    if (!ctx) return NULL;

    modbus_set_slave(ctx, slave_id);
    modbus_set_response_timeout(ctx, 5, 0);

    if (modbus_connect(ctx) == -1) {
        modbus_free(ctx);
        return NULL;
    }

    /* Read model string for auto-detection */
    char model[33];
    if (read_model_string(ctx, model, sizeof(model)) != 0) {
        modbus_close(ctx);
        modbus_free(ctx);
        return NULL;
    }

    const ups_driver_t *driver = detect_driver(model);
    if (!driver) {
        modbus_close(ctx);
        modbus_free(ctx);
        return NULL;
    }

    ups_t *ups = calloc(1, sizeof(*ups));
    if (!ups) {
        modbus_close(ctx);
        modbus_free(ctx);
        return NULL;
    }

    ups->ctx = ctx;
    ups->driver = driver;

    /* Read inventory immediately while connection is fresh */
    if (ups_read_inventory(ups, &ups->inventory) == 0)
        ups->has_inventory = 1;

    return ups;
}

void ups_close(ups_t *ups)
{
    if (ups) {
        if (ups->ctx) {
            modbus_close(ups->ctx);
            modbus_free(ups->ctx);
        }
        free(ups);
    }
}

/* --- Capability queries --- */

int ups_has_cap(const ups_t *ups, ups_cap_t cap)
{
    return (ups->driver->caps & (uint32_t)cap) != 0;
}

const char *ups_driver_name(const ups_t *ups)
{
    return ups->driver->name;
}

const ups_freq_setting_t *ups_get_freq_settings(const ups_t *ups, size_t *count)
{
    if (!ups_has_cap(ups, UPS_CAP_FREQ_TOLERANCE) || !ups->driver->freq_settings) {
        if (count) *count = 0;
        return NULL;
    }
    if (count) *count = ups->driver->freq_settings_count;
    return ups->driver->freq_settings;
}

const ups_freq_setting_t *ups_find_freq_setting(const ups_t *ups, const char *name)
{
    size_t count;
    const ups_freq_setting_t *settings = ups_get_freq_settings(ups, &count);
    if (!settings) return NULL;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(settings[i].name, name) == 0)
            return &settings[i];
    }
    return NULL;
}

const ups_freq_setting_t *ups_find_freq_value(const ups_t *ups, uint16_t value)
{
    size_t count;
    const ups_freq_setting_t *settings = ups_get_freq_settings(ups, &count);
    if (!settings) return NULL;
    for (size_t i = 0; i < count; i++) {
        if (settings[i].value == value)
            return &settings[i];
    }
    return NULL;
}

/* --- Read dispatch --- */

int ups_read_status(ups_t *ups, ups_data_t *data)
{
    if (!ups->driver->read_status) return UPS_ERR_NOT_SUPPORTED;
    return ups->driver->read_status(ups->ctx, data);
}

int ups_read_dynamic(ups_t *ups, ups_data_t *data)
{
    if (!ups->driver->read_dynamic) return UPS_ERR_NOT_SUPPORTED;
    return ups->driver->read_dynamic(ups->ctx, data);
}

int ups_read_inventory(ups_t *ups, ups_inventory_t *inv)
{
    if (!ups->driver->read_inventory) return UPS_ERR_NOT_SUPPORTED;
    return ups->driver->read_inventory(ups->ctx, inv);
}

int ups_read_thresholds(ups_t *ups, uint16_t *transfer_high, uint16_t *transfer_low)
{
    if (!ups->driver->read_thresholds) return UPS_ERR_NOT_SUPPORTED;
    return ups->driver->read_thresholds(ups->ctx, transfer_high, transfer_low);
}

/* --- Command dispatch --- */

int ups_cmd_shutdown(ups_t *ups)
{
    if (!ups->driver->cmd_shutdown) return UPS_ERR_NOT_SUPPORTED;
    return ups->driver->cmd_shutdown(ups->ctx);
}

int ups_cmd_battery_test(ups_t *ups)
{
    if (!ups->driver->cmd_battery_test) return UPS_ERR_NOT_SUPPORTED;
    return ups->driver->cmd_battery_test(ups->ctx);
}

int ups_cmd_runtime_cal(ups_t *ups)
{
    if (!ups->driver->cmd_runtime_cal) return UPS_ERR_NOT_SUPPORTED;
    return ups->driver->cmd_runtime_cal(ups->ctx);
}

int ups_cmd_abort_runtime_cal(ups_t *ups)
{
    if (!ups->driver->cmd_abort_runtime_cal) return UPS_ERR_NOT_SUPPORTED;
    return ups->driver->cmd_abort_runtime_cal(ups->ctx);
}

int ups_cmd_clear_faults(ups_t *ups)
{
    if (!ups->driver->cmd_clear_faults) return UPS_ERR_NOT_SUPPORTED;
    return ups->driver->cmd_clear_faults(ups->ctx);
}

int ups_cmd_mute_alarm(ups_t *ups)
{
    if (!ups->driver->cmd_mute_alarm) return UPS_ERR_NOT_SUPPORTED;
    return ups->driver->cmd_mute_alarm(ups->ctx);
}

int ups_cmd_cancel_mute(ups_t *ups)
{
    if (!ups->driver->cmd_cancel_mute) return UPS_ERR_NOT_SUPPORTED;
    return ups->driver->cmd_cancel_mute(ups->ctx);
}

int ups_cmd_beep_test(ups_t *ups)
{
    if (!ups->driver->cmd_beep_test) return UPS_ERR_NOT_SUPPORTED;
    return ups->driver->cmd_beep_test(ups->ctx);
}

int ups_cmd_bypass_enable(ups_t *ups)
{
    if (!ups->driver->cmd_bypass_enable) return UPS_ERR_NOT_SUPPORTED;
    return ups->driver->cmd_bypass_enable(ups->ctx);
}

int ups_cmd_bypass_disable(ups_t *ups)
{
    if (!ups->driver->cmd_bypass_disable) return UPS_ERR_NOT_SUPPORTED;
    return ups->driver->cmd_bypass_disable(ups->ctx);
}

int ups_cmd_set_freq_tolerance(ups_t *ups, uint16_t setting)
{
    if (!ups->driver->cmd_set_freq_tolerance) return UPS_ERR_NOT_SUPPORTED;
    return ups->driver->cmd_set_freq_tolerance(ups->ctx, setting);
}

/* --- String helpers (shared across all APC Modbus models) --- */

static const char *transfer_reasons[] = {
    "SystemInitialization", "HighInputVoltage", "LowInputVoltage",
    "DistortedInput", "RapidChangeOfInputVoltage", "HighInputFrequency",
    "LowInputFrequency", "FreqAndOrPhaseDifference", "AcceptableInput",
    "AutomaticTest", "TestEnded", "LocalUICommand", "ProtocolCommand",
    "LowBatteryVoltage", "GeneralError", "PowerSystemError",
    "BatterySystemError", "ErrorCleared", "AutomaticRestart",
    "DistortedInverterOutput", "InverterOutputAcceptable", "EPOInterface",
    "InputPhaseDeltaOutOfRange", "InputNeutralNotConnected", "ATSTransfer",
    "ConfigurationChange", "AlertAsserted", "AlertCleared",
    "PlugRatingExceeded", "OutletGroupStateChange", "FailureBypassExpired",
};

const char *ups_transfer_reason_str(uint16_t reason)
{
    if (reason < sizeof(transfer_reasons) / sizeof(transfer_reasons[0]))
        return transfer_reasons[reason];
    return "Unknown";
}

const char *ups_status_str(uint32_t status, char *buf, size_t len)
{
    buf[0] = '\0';
    struct { uint32_t bit; const char *name; } flags[] = {
        { UPS_ST_ONLINE,         "Online" },
        { UPS_ST_ON_BATTERY,     "OnBattery" },
        { UPS_ST_BYPASS,         "Bypass" },
        { UPS_ST_OUTPUT_OFF,     "OutputOff" },
        { UPS_ST_FAULT,          "Fault" },
        { UPS_ST_INPUT_BAD,      "InputBad" },
        { UPS_ST_TEST,           "SelfTest" },
        { UPS_ST_PENDING_ON,     "PendingOn" },
        { UPS_ST_SHUT_PENDING,   "ShutdownPending" },
        { UPS_ST_COMMANDED,      "Commanded" },
        { UPS_ST_HE_MODE,        "HighEfficiency" },
        { UPS_ST_INFO_ALERT,     "InfoAlert" },
        { UPS_ST_FAULT_STATE,    "FaultState" },
        { UPS_ST_MAINS_BAD,      "MainsBad" },
        { UPS_ST_FAULT_RECOVERY, "FaultRecovery" },
        { UPS_ST_OVERLOAD,       "Overload" },
    };
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
        if (status & flags[i].bit) {
            if (buf[0]) strncat(buf, " ", len - strlen(buf) - 1);
            strncat(buf, flags[i].name, len - strlen(buf) - 1);
        }
    }
    if (!buf[0]) strncpy(buf, "Unknown", len - 1);
    return buf;
}

const char *ups_efficiency_str(int16_t raw, char *buf, size_t len)
{
    if (raw >= 0) {
        snprintf(buf, len, "%.1f%%", raw / 128.0);
    } else {
        const char *reasons[] = {
            "NotAvailable", "LoadTooLow", "OutputOff", "OnBattery",
            "InBypass", "BatteryCharging", "PoorACInput", "BatteryDisconnected",
        };
        int idx = -raw - 1;
        if (idx >= 0 && idx < (int)(sizeof(reasons) / sizeof(reasons[0])))
            snprintf(buf, len, "%s", reasons[idx]);
        else
            snprintf(buf, len, "Unknown(%d)", raw);
    }
    return buf;
}
