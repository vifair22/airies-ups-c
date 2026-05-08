#include "ups_driver.h"
#include "ups.h"
#include "ups/ups_modbus.h"
#include <string.h>
#include <time.h>

/* --- SRT register map (990-9840, verified on SRT1000XLA FW 16.5) --- */

/* Status block: reg 0, 27 registers */
#define SRT_REG_STATUS       0
#define SRT_REG_STATUS_LEN   27

/* Dynamic block: reg 128, 44 registers */
#define SRT_REG_DYNAMIC      128
#define SRT_REG_DYNAMIC_LEN  44

/* Inventory block: reg 516, 80 registers */
#define SRT_REG_INVENTORY      516
#define SRT_REG_INVENTORY_LEN  80

/* Transfer thresholds */
#define SRT_REG_TRANSFER_HIGH  1026
#define SRT_REG_TRANSFER_LEN   2

/* Command registers */
#define SRT_REG_CMD_UPS        1536  /* uint32, FC16: bypass, clear faults */
#define SRT_REG_CMD_SHUTDOWN   1540  /* uint16, FC06: simple signaling shutdown */
#define SRT_REG_CMD_BATTEST    1541  /* uint16, FC06: battery test */
#define SRT_REG_CMD_RTCAL      1542  /* uint16, FC06: runtime calibration */
#define SRT_REG_CMD_UI         1543  /* uint16, FC06: beep, mute */

/* --- Transport helpers ---
 *
 * Drivers cast the opaque transport pointer to ups_mb_t* (the paced
 * Modbus wrapper). All Modbus I/O goes through ups_mb_read/write* so
 * the inter-op gap is enforced consistently across SMT and SRT — see
 * ups_modbus.h for the rationale. */

static ups_mb_t *mb(void *transport) { return (ups_mb_t *)transport; }

/* Decode an APC string register block: 2 chars per register, big-endian
 * within each register. Strings are not NUL-terminated and may pad with
 * 0x20 (per AN-176 §1.3.3) or 0x00 (observed in practice). Strip leading
 * NULs and trailing space/NUL. Mirrors smt_str_decode in ups_smt.c. */
static void srt_str_decode(const uint16_t *regs, int n, char *out, size_t outsz)
{
    size_t max = outsz - 1;
    if (max > (size_t)n * 2) max = (size_t)n * 2;
    size_t k = 0;
    for (int i = 0; i < n && k < max; i++) {
        char hi = (char)((regs[i] >> 8) & 0xFF);
        char lo = (char)(regs[i] & 0xFF);
        /* skip leading NULs only (some APC firmwares zero-pad the front
         * of a string field when the value is shorter than the slot) */
        if (k == 0) {
            if (hi) out[k++] = hi;
            if (lo && k < max) out[k++] = lo;
        } else {
            out[k++] = hi;
            if (k < max) out[k++] = lo;
        }
    }
    out[k] = '\0';
    while (k > 0 && (out[k-1] == ' ' || out[k-1] == '\0')) out[--k] = '\0';
}

/* --- Connection lifecycle --- */

static void *srt_connect(const ups_conn_params_t *params)
{
    if (params->type != UPS_CONN_SERIAL) return NULL;
    return ups_mb_new(params->serial.device, params->serial.baud,
                      params->serial.slave_id);
}

static void srt_disconnect(void *transport)
{
    ups_mb_free((ups_mb_t *)transport);
}

static int srt_detect(void *transport)
{
    /* Read model string from inventory block (reg 532, 16 regs = 32 chars) */
    uint16_t regs[16];
    if (ups_mb_read(mb(transport), 532, 16, regs) != 16)
        return 0;

    char model[33];
    memset(model, 0, sizeof(model));
    for (int i = 0; i < 16; i++) {
        model[i * 2]     = (char)((regs[i] >> 8) & 0xFF);
        model[i * 2 + 1] = (char)(regs[i] & 0xFF);
    }

    return strstr(model, "SRT") != NULL;
}

/* --- Reads --- */

/* Single-register read of the input transfer reason. Used by the monitor's
 * fast-poll thread to catch transitions that the slow main poll misses —
 * register 2 latches the cause briefly during mains events, then reverts
 * to AcceptableInput once input is good again. */
static int srt_read_transfer_reason(void *transport, uint16_t *out)
{
    uint16_t reg;
    if (ups_mb_read(mb(transport), SRT_REG_STATUS + 2, 1, &reg) != 1)
        return -1;
    *out = reg;
    return 0;
}

static int srt_read_status(void *transport, ups_data_t *data)
{
    uint16_t regs[SRT_REG_STATUS_LEN];
    if (ups_mb_read(mb(transport), SRT_REG_STATUS, SRT_REG_STATUS_LEN, regs) != SRT_REG_STATUS_LEN)
        return -1;

    data->status              = ((uint32_t)regs[0] << 16) | regs[1];
    data->transfer_reason     = regs[2];
    data->outlet_mog          = ((uint32_t)regs[3] << 16) | regs[4];
    data->outlet_sog0         = ((uint32_t)regs[6] << 16) | regs[7];
    data->outlet_sog1         = ((uint32_t)regs[9] << 16) | regs[10];
    data->sig_status          = regs[18];
    data->general_error       = regs[19];
    data->power_system_error  = ((uint32_t)regs[20] << 16) | regs[21];
    data->bat_system_error    = regs[22];
    data->bat_test_status     = regs[23];
    data->rt_cal_status       = regs[24];
    data->bat_lifetime_status = regs[25];
    data->ui_status           = regs[26];

    return 0;
}

static int srt_read_dynamic(void *transport, ups_data_t *data)
{
    uint16_t regs[SRT_REG_DYNAMIC_LEN];
    if (ups_mb_read(mb(transport), SRT_REG_DYNAMIC, SRT_REG_DYNAMIC_LEN, regs) != SRT_REG_DYNAMIC_LEN)
        return -1;

    data->runtime_sec      = ((uint32_t)regs[0] << 16) | regs[1];
    data->charge_pct       = regs[2] / 512.0;
    data->battery_voltage  = (int16_t)regs[3] / 32.0;
    data->load_pct         = regs[8] / 256.0;
    data->output_current   = regs[12] / 32.0;
    data->output_voltage   = regs[14] / 64.0;
    data->output_frequency = regs[16] / 128.0;
    data->output_energy_wh = ((uint32_t)regs[17] << 16) | regs[18];
    data->bypass_status    = regs[19];
    data->bypass_voltage   = regs[20] / 64.0;
    data->bypass_frequency = regs[21] / 128.0;
    data->input_status     = regs[22];
    data->input_voltage    = regs[23] / 64.0;
    {
        int16_t raw = (int16_t)regs[26];
        if (raw >= 0) {
            data->efficiency        = raw / 128.0;
            data->efficiency_reason = UPS_EFF_OK;
        } else {
            data->efficiency        = 0.0;
            data->efficiency_reason = (ups_eff_reason_t)(-raw);
        }
    }
    data->timer_shutdown   = (int16_t)regs[27];
    data->timer_start      = (int16_t)regs[28];
    data->timer_reboot     = ((int32_t)regs[29] << 16) | regs[30];

    return 0;
}

static int srt_read_inventory(void *transport, ups_inventory_t *inv)
{
    uint16_t regs[SRT_REG_INVENTORY_LEN];
    if (ups_mb_read(mb(transport), SRT_REG_INVENTORY, SRT_REG_INVENTORY_LEN, regs) != SRT_REG_INVENTORY_LEN)
        return -1;

    memset(inv, 0, sizeof(*inv));

    /* Block layout (offsets from base 516, per docs/reference/apc-srt-modbus.md):
     *   regs[ 0.. 7] firmware version (8 regs, 16 chars)
     *   regs[16..31] model name       (16 regs, 32 chars)
     *   regs[32..47] SKU              (16 regs, 32 chars)
     *   regs[48..55] serial number    (8 regs, 16 chars)
     *   regs[72]    nominal VA, [73] nominal W, [74] SOG relay config */
    srt_str_decode(regs +  0,  8, inv->firmware, sizeof(inv->firmware));
    srt_str_decode(regs + 16, 16, inv->model,    sizeof(inv->model));
    srt_str_decode(regs + 32, 16, inv->sku,      sizeof(inv->sku));
    srt_str_decode(regs + 48,  8, inv->serial,   sizeof(inv->serial));

    inv->nominal_va     = regs[72];
    inv->nominal_watts  = regs[73];
    inv->sog_config     = regs[74];

    return 0;
}

static int srt_read_thresholds(void *transport, uint16_t *transfer_high, uint16_t *transfer_low)
{
    uint16_t regs[SRT_REG_TRANSFER_LEN];
    if (ups_mb_read(mb(transport), SRT_REG_TRANSFER_HIGH, SRT_REG_TRANSFER_LEN, regs) != SRT_REG_TRANSFER_LEN)
        return -1;
    *transfer_high = regs[0];
    *transfer_low = regs[1];
    return 0;
}

/* --- Config register I/O --- */

static int srt_config_read(void *transport, const ups_config_reg_t *reg,
                           uint32_t *raw_value, char *str_buf, size_t str_bufsz)
{
    uint16_t regs[32];
    int n = reg->reg_count > 0 ? reg->reg_count : 1;
    if (n > 32) n = 32;

    if (ups_mb_read(mb(transport), reg->reg_addr, n, regs) != n)
        return UPS_ERR_IO;

    if (reg->type == UPS_CFG_STRING && str_buf) {
        srt_str_decode(regs, n, str_buf, str_bufsz);
    }

    if (raw_value) {
        /* APC convention: 32-bit values span two registers, MSB first. */
        if (n >= 2 && reg->type != UPS_CFG_STRING)
            *raw_value = ((uint32_t)regs[0] << 16) | (uint32_t)regs[1];
        else
            *raw_value = regs[0];
    }

    return UPS_OK;
}

static int srt_config_write(void *transport, const ups_config_reg_t *reg, uint16_t value)
{
    if (ups_mb_write(mb(transport), reg->reg_addr, value) != 1)
        return UPS_ERR_IO;

    /* Inter-write delay (UPS firmware requirement — 100ms minimum) */
    struct timespec delay = { .tv_sec = 0, .tv_nsec = 100000000 };
    nanosleep(&delay, NULL);

    /* Read back for verification */
    uint16_t readback;
    if (ups_mb_read(mb(transport), reg->reg_addr, 1, &readback) != 1)
        return UPS_ERR_IO;

    if (readback != value)
        return UPS_ERR_IO;

    return UPS_OK;
}

/* --- Commands --- */

static int srt_cmd_shutdown(void *transport)
{
    return ups_mb_write(mb(transport), SRT_REG_CMD_SHUTDOWN, 0x0001) == 1 ? 0 : -1;
}

static int srt_cmd_battery_test(void *transport)
{
    return ups_mb_write(mb(transport), SRT_REG_CMD_BATTEST, 0x0001) == 1 ? 0 : -1;
}

static int srt_cmd_runtime_cal(void *transport)
{
    return ups_mb_write(mb(transport), SRT_REG_CMD_RTCAL, 0x0001) == 1 ? 0 : -1;
}

static int srt_cmd_abort_runtime_cal(void *transport)
{
    return ups_mb_write(mb(transport), SRT_REG_CMD_RTCAL, 0x0002) == 1 ? 0 : -1;
}

static int srt_cmd_clear_faults(void *transport)
{
    uint16_t cmd[2] = { 0x0000, 0x0200 };
    return ups_mb_write_n(mb(transport), SRT_REG_CMD_UPS, 2, cmd) == 2 ? 0 : -1;
}

static int srt_cmd_mute_alarm(void *transport)
{
    return ups_mb_write(mb(transport), SRT_REG_CMD_UI, 0x0004) == 1 ? 0 : -1;
}

static int srt_cmd_cancel_mute(void *transport)
{
    return ups_mb_write(mb(transport), SRT_REG_CMD_UI, 0x0008) == 1 ? 0 : -1;
}

static int srt_cmd_beep_short(void *transport)
{
    return ups_mb_write(mb(transport), SRT_REG_CMD_UI, 0x0001) == 1 ? 0 : -1;
}

static int srt_cmd_beep_continuous(void *transport)
{
    return ups_mb_write(mb(transport), SRT_REG_CMD_UI, 0x0002) == 1 ? 0 : -1;
}

static int srt_cmd_bypass_enable(void *transport)
{
    uint16_t cmd[2] = { 0x0000, 0x0010 };
    return ups_mb_write_n(mb(transport), SRT_REG_CMD_UPS, 2, cmd) == 2 ? 0 : -1;
}

static int srt_cmd_bypass_disable(void *transport)
{
    uint16_t cmd[2] = { 0x0000, 0x0020 };
    return ups_mb_write_n(mb(transport), SRT_REG_CMD_UPS, 2, cmd) == 2 ? 0 : -1;
}

/* --- Frequency tolerance settings ---
 *
 * AN-176 / 990-9840B define seven values for AcceptableFrequencySetting_BF
 * (bits 0..6). On SRT1000XLA FW 16.5 only five are accepted; the SRT
 * firmware rejects bits 2 (Hz50_1_0 = value 4) and 5 (Hz60_1_0 = value
 * 32) with Modbus exception 0x04. See docs/reference/apc-srt-modbus.md
 * "Rejected Values (SRT1000XLA FW 16.5)" for reproduction details.
 *
 * Do not "fix" the gaps by re-adding 4/32 here without verifying on the
 * specific firmware in question — the same set is mirrored in
 * srt_freq_tol_opts below and surfaced via UPS_CAP_FREQ_TOLERANCE. */

static const ups_freq_setting_t srt_freq_settings[] = {
    { 1,  "auto",     "Automatic 50/60Hz (47-53, 57-63)" },
    { 2,  "hz50_0_1", "50 Hz +/- 0.1 Hz" },
    { 8,  "hz50_3_0", "50 Hz +/- 3.0 Hz" },
    { 16, "hz60_0_1", "60 Hz +/- 0.1 Hz" },
    { 64, "hz60_3_0", "60 Hz +/- 3.0 Hz" },
};

/* --- Config register descriptors ---
 *
 * BatteryTestIntervalSetting_BF: AN-176 defines six values (bits 0..5).
 * SRT firmware (verified on SRT1000XLA FW 16.5; same constraint
 * confirmed on SMT FW 04.1) rejects bits 2 (OnStartUpPlus7 = value 4)
 * and 3 (OnStartUpPlus14 = value 8) with Modbus exception 0x04. The
 * bits are spec-defined but apparently never wired into the operational
 * firmware. Strict-on validation in the registry blocks writes of these
 * values before they hit the wire. See docs/reference/apc-srt-modbus.md
 * "Battery Test Interval (Register 1024)" for the full table. */

static const ups_bitfield_opt_t srt_bat_test_opts[] = {
    { 1,  "never",           "Never" },
    { 2,  "onstart_only",    "Startup Only" },
    { 16, "onstart_7d",      "Startup + 7d Since Test" },
    { 32, "onstart_14d",     "Startup + 14d Since Test" },
};

static const ups_bitfield_opt_t srt_voltage_opts[] = {
    { 1,    "vac100",     "100 VAC" },
    { 2,    "vac120",     "120 VAC" },
    { 4,    "vac200",     "200 VAC" },
    { 8,    "vac208",     "208 VAC" },
    { 16,   "vac220",     "220 VAC" },
    { 32,   "vac230",     "230 VAC" },
    { 64,   "vac240",     "240 VAC" },
    { 2048, "vac110",     "110 VAC" },
};

static const ups_bitfield_opt_t srt_freq_tol_opts[] = {
    { 1,  "auto",     "Automatic 50/60Hz" },
    { 2,  "hz50_0_1", "50 Hz +/- 0.1 Hz" },
    { 8,  "hz50_3_0", "50 Hz +/- 3.0 Hz" },
    { 16, "hz60_0_1", "60 Hz +/- 0.1 Hz" },
    { 64, "hz60_3_0", "60 Hz +/- 3.0 Hz" },
};

/* --- FLAGS / BITFIELD options arrays for the comprehensive register dump.
 * Bit definitions transcribed from docs/reference/apc-srt-modbus.md (verified
 * against APC SRT1000XLA, FW UPS 16.5). Strict=0 throughout — these are
 * read-only diagnostics, not validation targets. */

static const ups_bitfield_opt_t srt_ups_status_opts[] = {
    { 0x00000002, "online",                "Online" },
    { 0x00000004, "on_battery",            "On Battery" },
    { 0x00000008, "output_on_bypass",      "Output on Bypass" },
    { 0x00000010, "output_off",            "Output Off" },
    { 0x00000020, "general_fault",         "General Fault" },
    { 0x00000040, "input_not_acceptable",  "Input Not Acceptable" },
    { 0x00000080, "self_test_in_progress", "Self Test in Progress" },
    { 0x00000100, "pending_output_on",     "Pending Output On" },
    { 0x00000200, "shutdown_pending",      "Shutdown Pending" },
    { 0x00000400, "commanded_bypass",      "Commanded Bypass" },
    { 0x00002000, "high_efficiency",       "High Efficiency Mode" },
    { 0x00004000, "informational_alert",   "Informational Alert" },
    { 0x00008000, "fault_state",           "Fault State" },
    { 0x00080000, "mains_bad_state",       "Mains Bad State" },
    { 0x00100000, "fault_recovery",        "Fault Recovery" },
    { 0x00200000, "overload",              "Overload" },
};

static const ups_bitfield_opt_t srt_transfer_reason_opts[] = {
    { 0,  "system_initialization",       "System Initialization" },
    { 1,  "high_input_voltage",          "High Input Voltage" },
    { 2,  "low_input_voltage",           "Low Input Voltage" },
    { 3,  "distorted_input",             "Distorted Input" },
    { 4,  "rapid_change_input_voltage",  "Rapid Change of Input Voltage" },
    { 5,  "high_input_frequency",        "High Input Frequency" },
    { 6,  "low_input_frequency",         "Low Input Frequency" },
    { 7,  "freq_or_phase_difference",    "Frequency and/or Phase Difference" },
    { 8,  "acceptable_input",            "Acceptable Input" },
    { 9,  "automatic_test",              "Automatic Test" },
    { 10, "test_ended",                  "Test Ended" },
    { 11, "local_ui_command",            "Local UI Command" },
    { 12, "protocol_command",            "Protocol Command" },
    { 13, "low_battery_voltage",         "Low Battery Voltage" },
    { 14, "general_error",               "General Error" },
    { 15, "power_system_error",          "Power System Error" },
    { 16, "battery_system_error",        "Battery System Error" },
    { 17, "error_cleared",               "Error Cleared" },
    { 18, "automatic_restart",           "Automatic Restart" },
    { 19, "distorted_inverter_output",   "Distorted Inverter Output" },
    { 20, "inverter_output_acceptable",  "Inverter Output Acceptable" },
    { 21, "epo_interface",               "EPO Interface" },
    { 22, "input_phase_delta_oor",       "Input Phase Delta Out of Range" },
    { 23, "input_neutral_not_connected", "Input Neutral Not Connected" },
    { 24, "ats_transfer",                "ATS Transfer" },
    { 25, "configuration_change",        "Configuration Change" },
    { 26, "alert_asserted",              "Alert Asserted" },
    { 27, "alert_cleared",               "Alert Cleared" },
    { 28, "plug_rating_exceeded",        "Plug Rating Exceeded" },
    { 29, "outlet_group_state_change",   "Outlet Group State Change" },
    { 30, "failure_bypass_expired",      "Failure Bypass Expired" },
};

static const ups_bitfield_opt_t srt_outlet_status_opts[] = {
    { 0x00000001, "state_on",                "State: ON" },
    { 0x00000002, "state_off",               "State: OFF" },
    { 0x00000004, "processing_reboot",       "Processing Reboot" },
    { 0x00000008, "processing_shutdown",     "Processing Shutdown" },
    { 0x00000010, "processing_sleep",        "Processing Sleep" },
    { 0x00000080, "pending_load_shed",       "Pending Load Shed" },
    { 0x00000100, "pending_on_delay",        "Pending On Delay" },
    { 0x00000200, "pending_off_delay",       "Pending Off Delay" },
    { 0x00000400, "pending_on_ac_presence",  "Pending On AC Presence" },
    { 0x00000800, "pending_on_min_runtime",  "Pending On Min Runtime" },
    { 0x00004000, "low_runtime",             "Low Runtime" },
};

static const ups_bitfield_opt_t srt_signaling_status_opts[] = {
    { 0x0001, "power_failure",     "Power Failure" },
    { 0x0002, "shutdown_imminent", "Shutdown Imminent" },
};

static const ups_bitfield_opt_t srt_general_error_opts[] = {
    { 0x0001, "site_wiring",            "Site Wiring" },
    { 0x0002, "eeprom",                 "EEPROM" },
    { 0x0004, "ad_converter",           "A/D Converter" },
    { 0x0008, "logic_power_supply",     "Logic Power Supply" },
    { 0x0010, "internal_communication", "Internal Communication" },
    { 0x0020, "ui_button",              "UI Button" },
    { 0x0080, "epo_active",             "EPO Active" },
    { 0x0100, "firmware_mismatch",      "Firmware Mismatch" },
    { 0x0200, "oscillator",             "Oscillator" },
    { 0x0400, "measurement_mismatch",   "Measurement Mismatch" },
    { 0x0800, "subsystem",              "Subsystem" },
};

static const ups_bitfield_opt_t srt_power_system_error_opts[] = {
    { 0x00000001, "output_overload",          "Output Overload" },
    { 0x00000002, "output_short_circuit",     "Output Short Circuit" },
    { 0x00000004, "output_overvoltage",       "Output Overvoltage" },
    { 0x00000008, "transformer_dc_imbalance", "Transformer DC Imbalance" },
    { 0x00000010, "overtemperature",          "Overtemperature" },
    { 0x00000020, "backfeed_relay",           "Backfeed Relay" },
    { 0x00000040, "avr_relay",                "AVR Relay" },
    { 0x00000080, "pfc_input_relay",          "PFC Input Relay" },
    { 0x00000100, "output_relay",             "Output Relay" },
    { 0x00000200, "bypass_relay",             "Bypass Relay" },
    { 0x00000400, "fan",                      "Fan" },
    { 0x00000800, "pfc",                      "PFC" },
    { 0x00001000, "dc_bus_overvoltage",       "DC Bus Overvoltage" },
    { 0x00002000, "inverter",                 "Inverter" },
    { 0x00004000, "over_current",             "Over Current" },
};

static const ups_bitfield_opt_t srt_battery_system_error_opts[] = {
    { 0x0001, "disconnected",       "Disconnected" },
    { 0x0002, "overvoltage",        "Overvoltage" },
    { 0x0004, "needs_replacement",  "Needs Replacement" },
    { 0x0008, "overtemp_critical",  "Overtemperature Critical" },
    { 0x0010, "charger",            "Charger" },
    { 0x0020, "temperature_sensor", "Temperature Sensor" },
    { 0x0040, "bus_soft_start",     "Bus Soft Start" },
    { 0x0080, "overtemp_warning",   "Overtemperature Warning" },
    { 0x0100, "general_error",      "General Error" },
    { 0x0200, "communication",      "Communication" },
};

static const ups_bitfield_opt_t srt_replace_battery_test_status_opts[] = {
    { 0x0001, "pending",         "Pending" },
    { 0x0002, "in_progress",     "In Progress" },
    { 0x0004, "passed",          "Passed" },
    { 0x0008, "failed",          "Failed" },
    { 0x0010, "refused",         "Refused" },
    { 0x0020, "aborted",         "Aborted" },
    { 0x0040, "source_protocol", "Source: Protocol" },
    { 0x0080, "source_local_ui", "Source: Local UI" },
    { 0x0100, "source_internal", "Source: Internal" },
};

static const ups_bitfield_opt_t srt_runtime_calibration_status_opts[] = {
    { 0x0001, "pending",                 "Pending" },
    { 0x0002, "in_progress",             "In Progress" },
    { 0x0004, "passed",                  "Passed" },
    { 0x0008, "failed",                  "Failed" },
    { 0x0010, "refused",                 "Refused" },
    { 0x0020, "aborted",                 "Aborted" },
    { 0x0040, "source_protocol",         "Source: Protocol" },
    { 0x0080, "source_local_ui",         "Source: Local UI" },
    { 0x1000, "load_change",             "Load Change" },
    { 0x2000, "ac_input_not_acceptable", "AC Input Not Acceptable" },
    { 0x4000, "load_too_low",            "Load Too Low" },
};

static const ups_bitfield_opt_t srt_battery_lifetime_status_opts[] = {
    { 0x0001, "ok",                    "Lifetime OK" },
    { 0x0002, "near_end",              "Lifetime Near End" },
    { 0x0004, "exceeded",              "Lifetime Exceeded" },
    { 0x0008, "near_end_acknowledged", "Lifetime Near End Acknowledged" },
    { 0x0010, "exceeded_acknowledged", "Lifetime Exceeded Acknowledged" },
};

static const ups_bitfield_opt_t srt_ui_status_opts[] = {
    { 0x0001, "continuous_test_in_progress", "Continuous Test in Progress" },
    { 0x0002, "audible_alarm_in_progress",   "Audible Alarm in Progress" },
    { 0x0004, "audible_alarm_muted",         "Audible Alarm Muted" },
    { 0x0008, "any_button_pressed_recently", "Any Button Pressed Recently" },
};

static const ups_bitfield_opt_t srt_input_status_opts[] = {
    { 0x0001, "acceptable",            "Acceptable" },
    { 0x0002, "pending_acceptable",    "Pending Acceptable" },
    { 0x0004, "voltage_too_low",       "Voltage Too Low" },
    { 0x0008, "voltage_too_high",      "Voltage Too High" },
    { 0x0010, "distorted",             "Distorted" },
    { 0x0020, "boost",                 "Boost" },
    { 0x0040, "trim",                  "Trim" },
    { 0x0080, "frequency_too_low",     "Frequency Too Low" },
    { 0x0100, "frequency_too_high",    "Frequency Too High" },
    { 0x0200, "freq_phase_not_locked", "Frequency and Phase Not Locked" },
    { 0x0400, "phase_delta_oor",       "Phase Delta Out of Range" },
    { 0x0800, "neutral_not_connected", "Neutral Not Connected" },
    { 0x8000, "powering_load",         "Powering Load" },
};

static const ups_bitfield_opt_t srt_sog_relay_config_opts[] = {
    { 0x0001, "mog_present",  "MOG Present" },
    { 0x0002, "sog0_present", "SOG0 Present" },
    { 0x0004, "sog1_present", "SOG1 Present" },
    { 0x0008, "sog2_present", "SOG2 Present" },
    { 0x0010, "sog3_present", "SOG3 Present" },
};

static const ups_config_reg_t srt_config_regs[] = {
    { "transfer_high", "Upper Acceptable Input Voltage", "V", "transfer",
      1026, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 110, 150 } },
    { "transfer_low", "Lower Acceptable Input Voltage", "V", "transfer",
      1027, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 80, 120 } },
    { "bat_test_interval", "Battery Test Interval", NULL, "battery",
      1024, 1, UPS_CFG_BITFIELD, 1, 1,
      .meta.bitfield = { srt_bat_test_opts,
                         sizeof(srt_bat_test_opts) / sizeof(srt_bat_test_opts[0]),
                         1 /* strict */ } },
    { "mog_turn_off_delay", "MOG Turn Off Countdown", "s", "outlet_delays",
      1029, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    { "mog_turn_on_delay", "MOG Turn On Countdown", "s", "outlet_delays",
      1030, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    { "sog0_turn_off_delay", "SOG0 Turn Off Countdown", "s", "outlet_delays",
      1034, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    { "sog0_turn_on_delay", "SOG0 Turn On Countdown", "s", "outlet_delays",
      1035, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    { "sog1_turn_off_delay", "SOG1 Turn Off Countdown", "s", "outlet_delays",
      1039, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    { "sog1_turn_on_delay", "SOG1 Turn On Countdown", "s", "outlet_delays",
      1040, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    { "mog_min_return_runtime", "MOG Minimum Return Runtime", "s", "outlet_delays",
      1033, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    { "sog0_min_return_runtime", "SOG0 Minimum Return Runtime", "s", "outlet_delays",
      1038, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    { "sog1_min_return_runtime", "SOG1 Minimum Return Runtime", "s", "outlet_delays",
      1043, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    { "mog_loadshed_config", "MOG Load Shed Config", NULL, "load_shed",
      1054, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 65535 } },
    { "sog0_loadshed_config", "SOG0 Load Shed Config", NULL, "load_shed",
      1056, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 65535 } },
    { "sog1_loadshed_config", "SOG1 Load Shed Config", NULL, "load_shed",
      1058, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 65535 } },
    { "mog_loadshed_runtime", "MOG Load Shed Runtime Threshold", "s", "load_shed",
      1072, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    { "sog0_loadshed_runtime", "SOG0 Load Shed Runtime Threshold", "s", "load_shed",
      1064, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    { "sog1_loadshed_runtime", "SOG1 Load Shed Runtime Threshold", "s", "load_shed",
      1065, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    { "mog_loadshed_time_on_bat", "MOG Load Shed Time on Battery", "s", "load_shed",
      1073, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    { "sog0_loadshed_time_on_bat", "SOG0 Load Shed Time on Battery", "s", "load_shed",
      1068, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    { "sog1_loadshed_time_on_bat", "SOG1 Load Shed Time on Battery", "s", "load_shed",
      1069, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    { "output_voltage_setting", "Output Voltage Setting", NULL, "output",
      592, 1, UPS_CFG_BITFIELD, 1, 0,
      .meta.bitfield = { srt_voltage_opts,
                         sizeof(srt_voltage_opts) / sizeof(srt_voltage_opts[0]),
                         1 /* strict */ } },
    { "freq_tolerance", "Output Frequency Tolerance", NULL, "output",
      593, 1, UPS_CFG_BITFIELD, 1, 1,
      .meta.bitfield = { srt_freq_tol_opts,
                         sizeof(srt_freq_tol_opts) / sizeof(srt_freq_tol_opts[0]),
                         1 /* strict */ } },
    { "manufacture_date", "Manufacture Date", "days since 2000-01-01", "info",
      591, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 65535 },
      .category = UPS_REG_CATEGORY_IDENTITY },
    { "battery_date", "Battery Install Date", "days since 2000-01-01", "info",
      595, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 65535 } },

    /* === Comprehensive register dump descriptors ===
     * Added 2026-04-25 to surface every documented Modbus register on
     * /api/about. Coverage is per docs/reference/apc-srt-modbus.md. Existing
     * entries above (transfer_*, bat_test_interval, outlet timings,
     * load_shed*, output_voltage_setting, freq_tolerance, dates) cover
     * the operator-tunable settings; the entries below cover the
     * read-only diagnostic / measurement / identity surface plus the
     * writable Names block. */

    /* Status block (registers 0-26) — DIAGNOSTIC */
    { "ups_status", "UPS Status", NULL, "status",
      0, 2, UPS_CFG_FLAGS, 1, 0,
      .meta.bitfield = { srt_ups_status_opts,
                         sizeof(srt_ups_status_opts) / sizeof(srt_ups_status_opts[0]),
                         0 },
      .category = UPS_REG_CATEGORY_DIAGNOSTIC },
    { "transfer_reason", "Last Transfer Reason", NULL, "status",
      2, 1, UPS_CFG_BITFIELD, 1, 0,
      .meta.bitfield = { srt_transfer_reason_opts,
                         sizeof(srt_transfer_reason_opts) / sizeof(srt_transfer_reason_opts[0]),
                         0 },
      .category = UPS_REG_CATEGORY_DIAGNOSTIC },
    { "mog_status", "MOG Status", NULL, "status",
      3, 2, UPS_CFG_FLAGS, 1, 0,
      .meta.bitfield = { srt_outlet_status_opts,
                         sizeof(srt_outlet_status_opts) / sizeof(srt_outlet_status_opts[0]),
                         0 },
      .category = UPS_REG_CATEGORY_DIAGNOSTIC },
    { "sog0_status", "SOG0 Status", NULL, "status",
      6, 2, UPS_CFG_FLAGS, 1, 0,
      .meta.bitfield = { srt_outlet_status_opts,
                         sizeof(srt_outlet_status_opts) / sizeof(srt_outlet_status_opts[0]),
                         0 },
      .category = UPS_REG_CATEGORY_DIAGNOSTIC },
    { "sog1_status", "SOG1 Status", NULL, "status",
      9, 2, UPS_CFG_FLAGS, 1, 0,
      .meta.bitfield = { srt_outlet_status_opts,
                         sizeof(srt_outlet_status_opts) / sizeof(srt_outlet_status_opts[0]),
                         0 },
      .category = UPS_REG_CATEGORY_DIAGNOSTIC },
    { "sog2_status", "SOG2 Status", NULL, "status",
      12, 2, UPS_CFG_FLAGS, 1, 0,
      .meta.bitfield = { srt_outlet_status_opts,
                         sizeof(srt_outlet_status_opts) / sizeof(srt_outlet_status_opts[0]),
                         0 },
      .category = UPS_REG_CATEGORY_DIAGNOSTIC },
    { "signaling_status", "Simple Signaling Status", NULL, "status",
      18, 1, UPS_CFG_FLAGS, 1, 0,
      .meta.bitfield = { srt_signaling_status_opts,
                         sizeof(srt_signaling_status_opts) / sizeof(srt_signaling_status_opts[0]),
                         0 },
      .category = UPS_REG_CATEGORY_DIAGNOSTIC },
    { "general_error", "General Error", NULL, "errors",
      19, 1, UPS_CFG_FLAGS, 1, 0,
      .meta.bitfield = { srt_general_error_opts,
                         sizeof(srt_general_error_opts) / sizeof(srt_general_error_opts[0]),
                         0 },
      .category = UPS_REG_CATEGORY_DIAGNOSTIC },
    { "power_system_error", "Power System Error", NULL, "errors",
      20, 2, UPS_CFG_FLAGS, 1, 0,
      .meta.bitfield = { srt_power_system_error_opts,
                         sizeof(srt_power_system_error_opts) / sizeof(srt_power_system_error_opts[0]),
                         0 },
      .category = UPS_REG_CATEGORY_DIAGNOSTIC },
    { "battery_system_error", "Battery System Error", NULL, "errors",
      22, 1, UPS_CFG_FLAGS, 1, 0,
      .meta.bitfield = { srt_battery_system_error_opts,
                         sizeof(srt_battery_system_error_opts) / sizeof(srt_battery_system_error_opts[0]),
                         0 },
      .category = UPS_REG_CATEGORY_DIAGNOSTIC },
    { "replace_battery_test_status", "Replace Battery Test Status", NULL, "status",
      23, 1, UPS_CFG_FLAGS, 1, 0,
      .meta.bitfield = { srt_replace_battery_test_status_opts,
                         sizeof(srt_replace_battery_test_status_opts) / sizeof(srt_replace_battery_test_status_opts[0]),
                         0 },
      .category = UPS_REG_CATEGORY_DIAGNOSTIC },
    { "runtime_calibration_status", "Runtime Calibration Status", NULL, "status",
      24, 1, UPS_CFG_FLAGS, 1, 0,
      .meta.bitfield = { srt_runtime_calibration_status_opts,
                         sizeof(srt_runtime_calibration_status_opts) / sizeof(srt_runtime_calibration_status_opts[0]),
                         0 },
      .category = UPS_REG_CATEGORY_DIAGNOSTIC },
    { "battery_lifetime_status", "Battery Lifetime Status", NULL, "status",
      25, 1, UPS_CFG_FLAGS, 1, 0,
      .meta.bitfield = { srt_battery_lifetime_status_opts,
                         sizeof(srt_battery_lifetime_status_opts) / sizeof(srt_battery_lifetime_status_opts[0]),
                         0 },
      .category = UPS_REG_CATEGORY_DIAGNOSTIC },
    { "ui_status", "UI Status", NULL, "status",
      26, 1, UPS_CFG_FLAGS, 1, 0,
      .meta.bitfield = { srt_ui_status_opts,
                         sizeof(srt_ui_status_opts) / sizeof(srt_ui_status_opts[0]),
                         0 },
      .category = UPS_REG_CATEGORY_DIAGNOSTIC },

    /* Dynamic block (registers 128-171) — MEASUREMENT */
    { "battery_runtime", "Battery Runtime", "s", "battery",
      128, 2, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 0, 0 },
      .category = UPS_REG_CATEGORY_MEASUREMENT },
    { "battery_charge_pct", "Battery Charge", "%", "battery",
      130, 1, UPS_CFG_SCALAR, 512, 0, .meta.scalar = { 0, 0, 0 },
      .category = UPS_REG_CATEGORY_MEASUREMENT },
    { "battery_voltage_pos", "Battery Positive Voltage", "V", "battery",
      131, 1, UPS_CFG_SCALAR, 32, 0, .meta.scalar = { 0, 0, 0 },
      .category = UPS_REG_CATEGORY_MEASUREMENT },
    { "battery_voltage_neg", "Battery Negative Voltage", "V", "battery",
      132, 1, UPS_CFG_SCALAR, 32, 0, .meta.scalar = { 0, 0, 0 },
      .category = UPS_REG_CATEGORY_MEASUREMENT },
    { "battery_internal_date", "Battery Internal Date", "days since 2000-01-01", "battery",
      133, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 0, 0 },
      .category = UPS_REG_CATEGORY_MEASUREMENT },
    { "battery_temperature", "Battery Temperature", "\xC2\xB0""C", "battery",
      135, 1, UPS_CFG_SCALAR, 128, 0, .meta.scalar = { 0, 0, 0 },
      .category = UPS_REG_CATEGORY_MEASUREMENT },
    { "output_load_pct", "Output Load", "%", "output",
      136, 1, UPS_CFG_SCALAR, 256, 0, .meta.scalar = { 0, 0, 0 },
      .category = UPS_REG_CATEGORY_MEASUREMENT },
    { "output_apparent_power_pct", "Output Apparent Power", "%", "output",
      138, 1, UPS_CFG_SCALAR, 256, 0, .meta.scalar = { 0, 0, 0 },
      .category = UPS_REG_CATEGORY_MEASUREMENT },
    { "output_current", "Output Current", "A", "output",
      140, 1, UPS_CFG_SCALAR, 32, 0, .meta.scalar = { 0, 0, 0 },
      .category = UPS_REG_CATEGORY_MEASUREMENT },
    { "output_voltage", "Output Voltage", "V", "output",
      142, 1, UPS_CFG_SCALAR, 64, 0, .meta.scalar = { 0, 0, 0 },
      .category = UPS_REG_CATEGORY_MEASUREMENT },
    { "output_frequency", "Output Frequency", "Hz", "output",
      144, 1, UPS_CFG_SCALAR, 128, 0, .meta.scalar = { 0, 0, 0 },
      .category = UPS_REG_CATEGORY_MEASUREMENT },
    { "output_energy", "Output Energy", "Wh", "output",
      145, 2, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 0, 0 },
      .category = UPS_REG_CATEGORY_MEASUREMENT },
    { "bypass_input_status", "Bypass Input Status", NULL, "bypass",
      147, 1, UPS_CFG_FLAGS, 1, 0,
      .meta.bitfield = { srt_input_status_opts,
                         sizeof(srt_input_status_opts) / sizeof(srt_input_status_opts[0]),
                         0 },
      .category = UPS_REG_CATEGORY_MEASUREMENT },
    { "bypass_voltage", "Bypass Voltage", "V", "bypass",
      148, 1, UPS_CFG_SCALAR, 64, 0, .meta.scalar = { 0, 0, 0 },
      .category = UPS_REG_CATEGORY_MEASUREMENT },
    { "bypass_frequency", "Bypass Frequency", "Hz", "bypass",
      149, 1, UPS_CFG_SCALAR, 128, 0, .meta.scalar = { 0, 0, 0 },
      .category = UPS_REG_CATEGORY_MEASUREMENT },
    { "input_status", "Input Status", NULL, "input",
      150, 1, UPS_CFG_FLAGS, 1, 0,
      .meta.bitfield = { srt_input_status_opts,
                         sizeof(srt_input_status_opts) / sizeof(srt_input_status_opts[0]),
                         0 },
      .category = UPS_REG_CATEGORY_MEASUREMENT },
    { "input_voltage", "Input Voltage", "V", "input",
      151, 1, UPS_CFG_SCALAR, 64, 0, .meta.scalar = { 0, 0, 0 },
      .category = UPS_REG_CATEGORY_MEASUREMENT,
      .has_sentinel = 1, .sentinel_value = 0xFFFF },
    { "efficiency", "Efficiency", "%", "ups",
      154, 1, UPS_CFG_SCALAR, 128, 0, .meta.scalar = { 0, 0, 0 },
      .category = UPS_REG_CATEGORY_MEASUREMENT,
      .has_sentinel = 1, .sentinel_value = 0xFFFF },
    { "shutdown_timer", "Shutdown Timer", "s", "timers",
      155, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 0, 0 },
      .category = UPS_REG_CATEGORY_MEASUREMENT,
      .has_sentinel = 1, .sentinel_value = 0xFFFF },
    { "start_timer", "Start Timer", "s", "timers",
      156, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 0, 0 },
      .category = UPS_REG_CATEGORY_MEASUREMENT,
      .has_sentinel = 1, .sentinel_value = 0xFFFF },
    { "reboot_timer", "Reboot Timer", "s", "timers",
      157, 2, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 0, 0 },
      .category = UPS_REG_CATEGORY_MEASUREMENT,
      .has_sentinel = 1, .sentinel_value = 0xFFFFFFFF },

    /* Inventory block (registers 516-595) — IDENTITY */
    { "firmware_version", "Firmware Version", NULL, "identity",
      516, 8, UPS_CFG_STRING, 1, 0, .meta.string = { .max_chars = 16 },
      .category = UPS_REG_CATEGORY_IDENTITY },
    { "model_name", "Model Name", NULL, "identity",
      532, 16, UPS_CFG_STRING, 1, 0, .meta.string = { .max_chars = 32 },
      .category = UPS_REG_CATEGORY_IDENTITY },
    { "sku", "SKU", NULL, "identity",
      548, 16, UPS_CFG_STRING, 1, 0, .meta.string = { .max_chars = 32 },
      .category = UPS_REG_CATEGORY_IDENTITY },
    { "serial_number", "Serial Number", NULL, "identity",
      564, 8, UPS_CFG_STRING, 1, 0, .meta.string = { .max_chars = 16 },
      .category = UPS_REG_CATEGORY_IDENTITY },
    { "battery_sku", "Battery SKU", NULL, "identity",
      572, 8, UPS_CFG_STRING, 1, 0, .meta.string = { .max_chars = 16 },
      .category = UPS_REG_CATEGORY_IDENTITY },
    { "external_battery_sku", "External Battery SKU", NULL, "identity",
      580, 8, UPS_CFG_STRING, 1, 0, .meta.string = { .max_chars = 16 },
      .category = UPS_REG_CATEGORY_IDENTITY },
    { "nominal_apparent_power", "Nominal Apparent Power", "VA", "identity",
      588, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 0, 0 },
      .category = UPS_REG_CATEGORY_IDENTITY },
    { "nominal_real_power", "Nominal Real Power", "W", "identity",
      589, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 0, 0 },
      .category = UPS_REG_CATEGORY_IDENTITY },
    { "sog_relay_config", "Outlet Group Relay Configuration", NULL, "identity",
      590, 1, UPS_CFG_FLAGS, 1, 0,
      .meta.bitfield = { srt_sog_relay_config_opts,
                         sizeof(srt_sog_relay_config_opts) / sizeof(srt_sog_relay_config_opts[0]),
                         0 },
      .category = UPS_REG_CATEGORY_IDENTITY },

    /* Names block (registers 596-635) — CONFIG, writable */
    { "ups_name", "UPS Name", NULL, "names",
      596, 8, UPS_CFG_STRING, 1, 1, .meta.string = { .max_chars = 16 } },
    { "mog_name", "MOG Name", NULL, "names",
      604, 8, UPS_CFG_STRING, 1, 1, .meta.string = { .max_chars = 16 } },
    { "sog0_name", "SOG0 Name", NULL, "names",
      612, 8, UPS_CFG_STRING, 1, 1, .meta.string = { .max_chars = 16 } },
    { "sog1_name", "SOG1 Name", NULL, "names",
      620, 8, UPS_CFG_STRING, 1, 1, .meta.string = { .max_chars = 16 } },
    { "sog2_name", "SOG2 Name", NULL, "names",
      628, 8, UPS_CFG_STRING, 1, 1, .meta.string = { .max_chars = 16 } },
};

/* --- Command descriptors --- */

static const ups_cmd_desc_t srt_commands[] = {
    { "shutdown", "Shutdown UPS", "Send the UPS shutdown command",
      "power", "Shutdown UPS?",
      "This sends the shutdown command directly to the UPS. Use the shutdown workflow for orchestrated multi-host shutdown.",
      UPS_CMD_SIMPLE, UPS_CMD_DANGER, UPS_CMD_IS_SHUTDOWN, 0,
      srt_cmd_shutdown, NULL },

    { "battery_test", "Battery Test", "Run a quick self-test to verify battery health",
      "diagnostics", "Start Battery Test?",
      "This will run a brief self-test where the UPS switches to battery power momentarily to verify battery health.",
      UPS_CMD_SIMPLE, UPS_CMD_DEFAULT, 0, 0,
      srt_cmd_battery_test, NULL },

    { "runtime_cal", "Runtime Calibration", "Deep discharge to recalibrate the runtime estimate",
      "diagnostics", "Start Runtime Calibration?",
      "Runtime calibration deeply discharges the battery to recalibrate the runtime estimate. The UPS will be on battery power for the entire duration. The battery will take a significant amount of time to recharge after calibration completes. If the battery is degraded, the UPS may not be able to sustain the load.",
      UPS_CMD_SIMPLE, UPS_CMD_WARN, 0, 0,
      srt_cmd_runtime_cal, NULL },

    { "abort_runtime_cal", "Abort Calibration", "Cancel a running runtime calibration",
      "diagnostics", "Abort Runtime Calibration?",
      "This will abort the current runtime calibration and return the UPS to normal operation.",
      UPS_CMD_SIMPLE, UPS_CMD_DEFAULT, 0, 0,
      srt_cmd_abort_runtime_cal, NULL },

    { "beep_short", "Short Beep", "Verify the audible alarm is functional",
      "diagnostics", "Short Beep Test?",
      "This will emit a brief beep to verify the audible alarm.",
      UPS_CMD_SIMPLE, UPS_CMD_DEFAULT, 0, 0,
      srt_cmd_beep_short, NULL },

    { "beep_continuous", "Continuous Beep", "Continuous beep and LED test",
      "diagnostics", "Continuous Beep Test?",
      "This starts a continuous beep and LED test. The alarm will sound until you stop it.",
      UPS_CMD_SIMPLE, UPS_CMD_WARN, 0, 0,
      srt_cmd_beep_continuous, NULL },

    { "mute", "Mute Alarm", "Silence the UPS audible alarm",
      "alarm", "Mute Alarm?",
      "This will silence the UPS audible alarm. The alarm will remain muted until a new alarm condition occurs or you unmute it.",
      UPS_CMD_SIMPLE, UPS_CMD_DEFAULT, UPS_CMD_IS_MUTE, 0,
      srt_cmd_mute_alarm, NULL },

    { "unmute", "Unmute Alarm", "Re-enable the UPS audible alarm",
      "alarm", "Unmute Alarm?",
      "This will re-enable the UPS audible alarm. Any active alarm conditions will immediately sound.",
      UPS_CMD_SIMPLE, UPS_CMD_DEFAULT, 0, 0,
      srt_cmd_cancel_mute, NULL },

    { "clear_faults", "Clear Faults", "Reset latched fault flags",
      "alarm", "Clear Fault Flags?",
      "This will reset all latched fault indicators on the UPS.",
      UPS_CMD_SIMPLE, UPS_CMD_DEFAULT, 0, 0,
      srt_cmd_clear_faults, NULL },

    { "bypass", "Bypass Mode",
      "Routes utility power directly to the output, bypassing the UPS power conditioning. Used for UPS maintenance or to reduce heat and power consumption.",
      "power", "Toggle Bypass?",
      "Enabling bypass routes utility power directly to the output without conditioning. Disabling returns the UPS to normal operation.",
      UPS_CMD_TOGGLE, UPS_CMD_WARN, 0, UPS_ST_BYPASS,
      srt_cmd_bypass_enable, srt_cmd_bypass_disable },
};

/* --- Driver definition --- */

const ups_driver_t ups_driver_srt = {
    .name                = "srt",
    .conn_type           = UPS_CONN_SERIAL,
    .topology            = UPS_TOPO_ONLINE_DOUBLE,
    .connect             = srt_connect,
    .disconnect          = srt_disconnect,
    .detect              = srt_detect,
    .caps                = UPS_CAP_SHUTDOWN | UPS_CAP_BATTERY_TEST | UPS_CAP_RUNTIME_CAL |
                           UPS_CAP_CLEAR_FAULTS | UPS_CAP_MUTE | UPS_CAP_BEEP |
                           UPS_CAP_BYPASS | UPS_CAP_FREQ_TOLERANCE | UPS_CAP_HE_MODE,
    .freq_settings       = srt_freq_settings,
    .freq_settings_count = sizeof(srt_freq_settings) / sizeof(srt_freq_settings[0]),
    .config_regs         = srt_config_regs,
    .config_regs_count   = sizeof(srt_config_regs) / sizeof(srt_config_regs[0]),
    .read_status         = srt_read_status,
    .read_dynamic        = srt_read_dynamic,
    .read_inventory      = srt_read_inventory,
    .read_thresholds     = srt_read_thresholds,
    .read_transfer_reason = srt_read_transfer_reason,
    .commands            = srt_commands,
    .commands_count      = sizeof(srt_commands) / sizeof(srt_commands[0]),
    .config_read         = srt_config_read,
    .config_write        = srt_config_write,
};
