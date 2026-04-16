#include "ups.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- Driver registry --- */

extern const ups_driver_t ups_driver_srt;
extern const ups_driver_t ups_driver_smt;

const ups_driver_t *ups_drivers[] = {
    &ups_driver_srt,
    &ups_driver_smt,
    NULL,
};

/* --- Connection + auto-detect ---
 *
 * For each registered driver matching the connection type:
 *   1. Call driver->connect(params) to open the transport
 *   2. Call driver->detect(transport) to check if this UPS matches
 *   3. If detect returns 1, we have our driver. Otherwise disconnect and try next.
 */

ups_t *ups_connect(const ups_conn_params_t *params)
{
    if (!params) return NULL;

    for (int i = 0; ups_drivers[i] != NULL; i++) {
        const ups_driver_t *drv = ups_drivers[i];

        /* Skip drivers that don't match the connection type */
        if (drv->conn_type != params->type)
            continue;

        if (!drv->connect || !drv->detect)
            continue;

        void *transport = drv->connect(params);
        if (!transport)
            continue;

        if (!drv->detect(transport)) {
            if (drv->disconnect) drv->disconnect(transport);
            continue;
        }

        /* Match — build the context */
        ups_t *ups = calloc(1, sizeof(*ups));
        if (!ups) {
            if (drv->disconnect) drv->disconnect(transport);
            return NULL;
        }

        ups->transport = transport;
        ups->driver = drv;
        ups->params = *params;
        /* Deep-copy string pointers so params survive the caller's stack */
        if (params->type == UPS_CONN_SERIAL && params->serial.device) {
            snprintf(ups->_device_buf, sizeof(ups->_device_buf), "%s", params->serial.device);
            ups->params.serial.device = ups->_device_buf;
        }
        if (params->type == UPS_CONN_USB && params->usb.serial) {
            snprintf(ups->_serial_buf, sizeof(ups->_serial_buf), "%s", params->usb.serial);
            ups->params.usb.serial = ups->_serial_buf;
        }
        if (params->type == UPS_CONN_TCP && params->tcp.host) {
            snprintf(ups->_host_buf, sizeof(ups->_host_buf), "%s", params->tcp.host);
            ups->params.tcp.host = ups->_host_buf;
        }

        pthread_mutex_init(&ups->cmd_mutex, NULL);

        /* Read inventory immediately while connection is fresh */
        if (ups_read_inventory(ups, &ups->inventory) == 0)
            ups->has_inventory = 1;

        return ups;
    }

    return NULL;
}

void ups_close(ups_t *ups)
{
    if (ups) {
        if (ups->transport && ups->driver->disconnect)
            ups->driver->disconnect(ups->transport);
        pthread_mutex_destroy(&ups->cmd_mutex);
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

ups_topology_t ups_topology(const ups_t *ups)
{
    return ups->driver->topology;
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

/* --- Error recovery --- */

#define MAX_CONSECUTIVE_ERRORS 5

static void ups_clear_errors(ups_t *ups)
{
    ups->consecutive_errors = 0;
}

/* After repeated failures, disconnect and reconnect through the driver. */
static void ups_handle_error(ups_t *ups)
{
    ups->consecutive_errors++;

    if (ups->consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
        if (ups->driver->disconnect)
            ups->driver->disconnect(ups->transport);
        ups->transport = NULL;

        if (ups->driver->connect) {
            ups->transport = ups->driver->connect(&ups->params);
            if (ups->transport)
                ups->consecutive_errors = 0;
        }
    }
}

/* --- Read dispatch --- */

int ups_read_status(ups_t *ups, ups_data_t *data)
{
    if (!ups->driver->read_status) return UPS_ERR_NOT_SUPPORTED;
    if (!ups->transport) return UPS_ERR_IO;
    pthread_mutex_lock(&ups->cmd_mutex);
    int rc = ups->driver->read_status(ups->transport, data);
    if (rc != 0) ups_handle_error(ups); else ups_clear_errors(ups);
    pthread_mutex_unlock(&ups->cmd_mutex);
    return rc;
}

int ups_read_dynamic(ups_t *ups, ups_data_t *data)
{
    if (!ups->driver->read_dynamic) return UPS_ERR_NOT_SUPPORTED;
    if (!ups->transport) return UPS_ERR_IO;
    pthread_mutex_lock(&ups->cmd_mutex);
    int rc = ups->driver->read_dynamic(ups->transport, data);
    if (rc != 0) ups_handle_error(ups); else ups_clear_errors(ups);
    pthread_mutex_unlock(&ups->cmd_mutex);
    return rc;
}

int ups_read_inventory(ups_t *ups, ups_inventory_t *inv)
{
    if (!ups->driver->read_inventory) return UPS_ERR_NOT_SUPPORTED;
    if (!ups->transport) return UPS_ERR_IO;
    pthread_mutex_lock(&ups->cmd_mutex);
    int rc = ups->driver->read_inventory(ups->transport, inv);
    pthread_mutex_unlock(&ups->cmd_mutex);
    return rc;
}

int ups_read_thresholds(ups_t *ups, uint16_t *transfer_high, uint16_t *transfer_low)
{
    if (!ups->driver->read_thresholds) return UPS_ERR_NOT_SUPPORTED;
    if (!ups->transport) return UPS_ERR_IO;
    pthread_mutex_lock(&ups->cmd_mutex);
    int rc = ups->driver->read_thresholds(ups->transport, transfer_high, transfer_low);
    pthread_mutex_unlock(&ups->cmd_mutex);
    return rc;
}

/* --- Command dispatch ---
 * All commands hold the mutex. Post-command settle delay gives the UPS
 * time to process before the monitor's next read. */

static void post_command_settle(void)
{
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 200000000 }; /* 200ms */
    nanosleep(&ts, NULL);
}

/* --- Command descriptor access --- */

const ups_cmd_desc_t *ups_get_commands(const ups_t *ups, size_t *count)
{
    if (!ups->driver->commands) {
        if (count) *count = 0;
        return NULL;
    }
    if (count) *count = ups->driver->commands_count;
    return ups->driver->commands;
}

const ups_cmd_desc_t *ups_find_command(const ups_t *ups, const char *name)
{
    size_t count;
    const ups_cmd_desc_t *cmds = ups_get_commands(ups, &count);
    if (!cmds) return NULL;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(cmds[i].name, name) == 0)
            return &cmds[i];
    }
    return NULL;
}

const ups_cmd_desc_t *ups_find_command_flag(const ups_t *ups, uint32_t flag)
{
    size_t count;
    const ups_cmd_desc_t *cmds = ups_get_commands(ups, &count);
    if (!cmds) return NULL;
    for (size_t i = 0; i < count; i++) {
        if (cmds[i].flags & flag)
            return &cmds[i];
    }
    return NULL;
}

int ups_cmd_execute(ups_t *ups, const char *name, int is_off)
{
    const ups_cmd_desc_t *cmd = ups_find_command(ups, name);
    if (!cmd) return UPS_ERR_NOT_SUPPORTED;

    int (*fn)(void *) = is_off ? cmd->execute_off : cmd->execute;
    if (!fn) return UPS_ERR_NOT_SUPPORTED;
    if (!ups->transport) return UPS_ERR_IO;

    pthread_mutex_lock(&ups->cmd_mutex);
    int rc = fn(ups->transport);
    post_command_settle();
    pthread_mutex_unlock(&ups->cmd_mutex);
    return rc;
}

/* --- Config register access ---
 * Delegates to driver's config_read/config_write if provided.
 * Falls back to UPS_ERR_NOT_SUPPORTED if the driver doesn't implement them. */

const ups_config_reg_t *ups_get_config_regs(const ups_t *ups, size_t *count)
{
    if (!ups->driver->config_regs) {
        if (count) *count = 0;
        return NULL;
    }
    if (count) *count = ups->driver->config_regs_count;
    return ups->driver->config_regs;
}

const ups_config_reg_t *ups_find_config_reg(const ups_t *ups, const char *name)
{
    size_t count;
    const ups_config_reg_t *regs = ups_get_config_regs(ups, &count);
    if (!regs) return NULL;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(regs[i].name, name) == 0)
            return &regs[i];
    }
    return NULL;
}

int ups_config_read(ups_t *ups, const ups_config_reg_t *reg,
                    uint16_t *raw_value, char *str_buf, size_t str_bufsz)
{
    if (!ups->driver->config_read) return UPS_ERR_NOT_SUPPORTED;
    if (!ups->transport) return UPS_ERR_IO;
    pthread_mutex_lock(&ups->cmd_mutex);
    int rc = ups->driver->config_read(ups->transport, reg, raw_value, str_buf, str_bufsz);
    if (rc != 0) ups_handle_error(ups); else ups_clear_errors(ups);
    pthread_mutex_unlock(&ups->cmd_mutex);
    return rc;
}

int ups_config_write(ups_t *ups, const ups_config_reg_t *reg, uint16_t value)
{
    if (!reg->writable) return UPS_ERR_NOT_SUPPORTED;
    if (!ups->driver->config_write) return UPS_ERR_NOT_SUPPORTED;
    if (!ups->transport) return UPS_ERR_IO;
    pthread_mutex_lock(&ups->cmd_mutex);
    int rc = ups->driver->config_write(ups->transport, reg, value);
    post_command_settle();
    pthread_mutex_unlock(&ups->cmd_mutex);
    return rc;
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
