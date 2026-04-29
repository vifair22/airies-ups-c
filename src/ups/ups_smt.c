#include "ups_driver.h"
#include "ups.h"
#include <modbus/modbus.h>
#include <string.h>
#include <time.h>

/* --- APC Smart-UPS SMT (line-interactive) Modbus RTU driver ---
 *
 * Verified against APC SMT1500RM2UC (FW UPS 04.1, ModbusMapID 00.5) on
 * /dev/ttyUSB0 at 9600 8N1, slave 1. The full register table and the
 * real-life quirks this driver works around live in
 * docs/reference/apc-smt-modbus.md.
 *
 * Differences from the SRT driver in the same family:
 *
 *   - Detect via SKU_STR (reg 548), not Model_STR. Live SMT1500RM2UC
 *     reports Model_STR = "Smart-UPS 1500" with no family prefix; the
 *     SRT driver gets away with reading Model_STR because SRT model
 *     strings include "SRT".
 *   - No bypass: SMT is line-interactive. Status bit 3 / 10 are never
 *     set, regs 147/148/149 read sentinels. Driver does not populate
 *     ups_data.bypass_*, does not advertise UPS_CAP_BYPASS, does not
 *     expose bypass commands.
 *   - No frequency tolerance: register 593 is SRT/SURTD-only (it reads
 *     0x0000 on SMT, defined-but-inapplicable). Driver does not
 *     advertise UPS_CAP_FREQ_TOLERANCE.
 *   - SMT-only Sensitivity setting at reg 1028 (Normal/Reduced/Low) is
 *     exposed as a config descriptor.
 *   - SOG count varies per SKU (the SMT1500RM2UC has only MOG + SOG0).
 *     resolve_config_regs reads reg 590 at connect time and drops
 *     descriptors for SOG groups that aren't physically present, so
 *     the UI never offers controls for non-existent outlets.
 *
 * Same as SRT (and the rest of the SMX/SMT/SURTD/SRT family per
 * 990-9840B): block layout, scaling factors, command register addresses,
 * inventory structure. */

/* --- Register block addresses (990-9840B) --- */
#define SMT_REG_STATUS           0
#define SMT_REG_STATUS_LEN       27
#define SMT_REG_DYNAMIC          128
#define SMT_REG_DYNAMIC_LEN      44
#define SMT_REG_INVENTORY        516
#define SMT_REG_INVENTORY_LEN    80
#define SMT_REG_SKU              548   /* used by detect() */
#define SMT_REG_SKU_LEN          16
#define SMT_REG_SOG_CONFIG       590   /* used by resolve_config_regs() */
#define SMT_REG_TRANSFER_HIGH    1026
#define SMT_REG_TRANSFER_LEN     2
#define SMT_REG_CMD_UPS          1536  /* uint32, FC16 */
#define SMT_REG_CMD_SHUTDOWN     1540  /* uint16, FC06 (SimpleSignaling) */
#define SMT_REG_CMD_BATTEST      1541  /* uint16, FC06 */
#define SMT_REG_CMD_RTCAL        1542  /* uint16, FC06 */
#define SMT_REG_CMD_UI           1543  /* uint16, FC06 (beep/mute) */

/* SOGRelayConfigSetting_BF bits (reg 590) */
#define SMT_SOG_HAS_MOG          (1 << 0)
#define SMT_SOG_HAS_SOG0         (1 << 1)
#define SMT_SOG_HAS_SOG1         (1 << 2)
#define SMT_SOG_HAS_SOG2         (1 << 3)

/* --- Transport helpers --- */

static modbus_t *mb(void *transport) { return (modbus_t *)transport; }

/* Decode an APC string register block: 2 chars per register, big-endian
 * within each register. Strings are not NUL-terminated and pad with
 * either 0x20 (per spec) or 0x00 (observed in SMT inventory) — strip
 * both, plus leading 0x00 fillers some firmwares emit. */
static void smt_str_decode(const uint16_t *regs, int n, char *out, size_t outsz)
{
    size_t max = outsz - 1;
    if (max > (size_t)n * 2) max = (size_t)n * 2;
    size_t k = 0;
    for (int i = 0; i < n && k < max; i++) {
        char hi = (char)((regs[i] >> 8) & 0xFF);
        char lo = (char)(regs[i] & 0xFF);
        /* skip leading NULs only (some APC firmwares pad the front of
         * a string field with zeros when it's shorter than the slot) */
        if (k == 0) {
            if (hi) out[k++] = hi;
            if (lo && k < max) out[k++] = lo;
        } else {
            out[k++] = hi;
            if (k < max) out[k++] = lo;
        }
    }
    out[k] = '\0';
    /* strip trailing spaces and NULs */
    while (k > 0 && (out[k-1] == ' ' || out[k-1] == '\0')) out[--k] = '\0';
}

/* --- Connection lifecycle --- */

static void *smt_connect(const ups_conn_params_t *params)
{
    if (params->type != UPS_CONN_SERIAL) return NULL;

    modbus_t *ctx = modbus_new_rtu(params->serial.device, params->serial.baud,
                                   'N', 8, 1);
    if (!ctx) return NULL;

    modbus_set_slave(ctx, params->serial.slave_id);
    modbus_set_response_timeout(ctx, 5, 0);

    if (modbus_connect(ctx) == -1) {
        modbus_free(ctx);
        return NULL;
    }

    return ctx;
}

static void smt_disconnect(void *transport)
{
    if (transport) {
        modbus_close(mb(transport));
        modbus_free(mb(transport));
    }
}

/* Detect: read SKU_STR (reg 548, 16 regs = 32 chars) and substring-match
 * "SMT". Model_STR alone does not contain the family prefix on the SMT
 * line — see docs/reference/apc-smt-modbus.md "Quirk — driver must detect via
 * SKU, not Model". */
static int smt_detect(void *transport)
{
    uint16_t regs[SMT_REG_SKU_LEN];
    if (modbus_read_registers(mb(transport), SMT_REG_SKU, SMT_REG_SKU_LEN, regs)
        != SMT_REG_SKU_LEN)
        return 0;

    char sku[SMT_REG_SKU_LEN * 2 + 1];
    smt_str_decode(regs, SMT_REG_SKU_LEN, sku, sizeof(sku));

    return strstr(sku, "SMT") != NULL;
}

/* --- Reads --- */

/* Single-register read of the input transfer reason. Used by the monitor's
 * fast-poll thread; see srt_read_transfer_reason for the rationale. */
static int smt_read_transfer_reason(void *transport, uint16_t *out)
{
    uint16_t reg;
    if (modbus_read_registers(mb(transport), SMT_REG_STATUS + 2, 1, &reg) != 1)
        return -1;
    *out = reg;
    return 0;
}

static int smt_read_status(void *transport, ups_data_t *data)
{
    uint16_t regs[SMT_REG_STATUS_LEN];
    if (modbus_read_registers(mb(transport), SMT_REG_STATUS, SMT_REG_STATUS_LEN, regs)
        != SMT_REG_STATUS_LEN)
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

static int smt_read_dynamic(void *transport, ups_data_t *data)
{
    uint16_t regs[SMT_REG_DYNAMIC_LEN];
    if (modbus_read_registers(mb(transport), SMT_REG_DYNAMIC, SMT_REG_DYNAMIC_LEN, regs)
        != SMT_REG_DYNAMIC_LEN)
        return -1;

    data->runtime_sec      = ((uint32_t)regs[0] << 16) | regs[1];
    data->charge_pct       = regs[2] / 512.0;
    data->battery_voltage  = (int16_t)regs[3] / 32.0;
    data->load_pct         = regs[8] / 256.0;
    data->output_current   = regs[12] / 32.0;
    data->output_voltage   = regs[14] / 64.0;
    data->output_frequency = regs[16] / 128.0;
    data->output_energy_wh = ((uint32_t)regs[17] << 16) | regs[18];
    /* regs[19..21]: bypass fields — not applicable on SMT, leave unset */
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

static int smt_read_inventory(void *transport, ups_inventory_t *inv)
{
    uint16_t regs[SMT_REG_INVENTORY_LEN];
    if (modbus_read_registers(mb(transport), SMT_REG_INVENTORY, SMT_REG_INVENTORY_LEN, regs)
        != SMT_REG_INVENTORY_LEN)
        return -1;

    memset(inv, 0, sizeof(*inv));

    smt_str_decode(regs +  0,  8, inv->firmware, sizeof(inv->firmware));
    smt_str_decode(regs + 16, 16, inv->model,    sizeof(inv->model));
    smt_str_decode(regs + 32, 16, inv->sku,      sizeof(inv->sku));
    smt_str_decode(regs + 48,  8, inv->serial,   sizeof(inv->serial));

    inv->nominal_va    = regs[72];
    inv->nominal_watts = regs[73];
    inv->sog_config    = regs[74];

    return 0;
}

static int smt_read_thresholds(void *transport, uint16_t *transfer_high, uint16_t *transfer_low)
{
    uint16_t regs[SMT_REG_TRANSFER_LEN];
    if (modbus_read_registers(mb(transport), SMT_REG_TRANSFER_HIGH,
                              SMT_REG_TRANSFER_LEN, regs) != SMT_REG_TRANSFER_LEN)
        return -1;
    *transfer_high = regs[0];
    *transfer_low  = regs[1];
    return 0;
}

/* --- Config register I/O --- */

static int smt_config_read(void *transport, const ups_config_reg_t *reg,
                           uint32_t *raw_value, char *str_buf, size_t str_bufsz)
{
    uint16_t regs[32];
    int n = reg->reg_count > 0 ? reg->reg_count : 1;
    if (n > 32) n = 32;

    if (modbus_read_registers(mb(transport), reg->reg_addr, n, regs) != n)
        return UPS_ERR_IO;

    if (reg->type == UPS_CFG_STRING && str_buf) {
        smt_str_decode(regs, n, str_buf, str_bufsz);
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

static int smt_config_write(void *transport, const ups_config_reg_t *reg, uint16_t value)
{
    if (modbus_write_register(mb(transport), reg->reg_addr, value) != 1)
        return UPS_ERR_IO;

    /* APC firmware needs ~100 ms before the value is observable on a
     * read-back. Same delay the SRT driver applies. */
    struct timespec delay = { .tv_sec = 0, .tv_nsec = 100000000 };
    nanosleep(&delay, NULL);

    uint16_t readback;
    if (modbus_read_registers(mb(transport), reg->reg_addr, 1, &readback) != 1)
        return UPS_ERR_IO;

    if (readback != value)
        return UPS_ERR_IO;

    return UPS_OK;
}

/* --- Commands ---
 * All return 0 on success, -1 on Modbus failure. The registry holds
 * cmd_mutex around the call and sleeps 200 ms after return. */

static int smt_cmd_shutdown(void *transport)
{
    return modbus_write_register(mb(transport), SMT_REG_CMD_SHUTDOWN, 0x0001) == 1 ? 0 : -1;
}

static int smt_cmd_battery_test(void *transport)
{
    return modbus_write_register(mb(transport), SMT_REG_CMD_BATTEST, 0x0001) == 1 ? 0 : -1;
}

static int smt_cmd_runtime_cal(void *transport)
{
    return modbus_write_register(mb(transport), SMT_REG_CMD_RTCAL, 0x0001) == 1 ? 0 : -1;
}

static int smt_cmd_abort_runtime_cal(void *transport)
{
    return modbus_write_register(mb(transport), SMT_REG_CMD_RTCAL, 0x0002) == 1 ? 0 : -1;
}

static int smt_cmd_clear_faults(void *transport)
{
    /* UPSCommand_BF bit 9 = ClearFaults; written via FC16 as a 32-bit BF
     * with the high 16 bits zero, low 16 bits = 0x0200. */
    uint16_t cmd[2] = { 0x0000, 0x0200 };
    return modbus_write_registers(mb(transport), SMT_REG_CMD_UPS, 2, cmd) == 2 ? 0 : -1;
}

static int smt_cmd_mute_alarm(void *transport)
{
    return modbus_write_register(mb(transport), SMT_REG_CMD_UI, 0x0004) == 1 ? 0 : -1;
}

static int smt_cmd_cancel_mute(void *transport)
{
    return modbus_write_register(mb(transport), SMT_REG_CMD_UI, 0x0008) == 1 ? 0 : -1;
}

static int smt_cmd_beep_short(void *transport)
{
    return modbus_write_register(mb(transport), SMT_REG_CMD_UI, 0x0001) == 1 ? 0 : -1;
}

static int smt_cmd_beep_continuous(void *transport)
{
    return modbus_write_register(mb(transport), SMT_REG_CMD_UI, 0x0002) == 1 ? 0 : -1;
}

/* --- Config register descriptors ---
 *
 * Indices matter: smt_resolve_config_regs uses them to decide which
 * descriptors survive into ups_t->resolved_regs. Adding/reordering
 * entries means updating the CFG_* constants and the resolve switch
 * below. The compile-time _Static_assert guards a single sentinel. */

enum {
    CFG_TRANSFER_HIGH = 0,
    CFG_TRANSFER_LOW,
    CFG_BAT_TEST_INTERVAL,
    CFG_SENSITIVITY,
    CFG_OUTPUT_VOLTAGE_SETTING,
    CFG_MANUFACTURE_DATE,
    CFG_BATTERY_DATE,
    /* MOG */
    CFG_MOG_TURN_OFF_DELAY,
    CFG_MOG_TURN_ON_DELAY,
    CFG_MOG_MIN_RETURN_RUNTIME,
    CFG_MOG_LOADSHED_CONFIG,
    CFG_MOG_LOADSHED_RUNTIME,
    CFG_MOG_LOADSHED_TIME_ON_BAT,
    /* SOG0 */
    CFG_SOG0_TURN_OFF_DELAY,
    CFG_SOG0_TURN_ON_DELAY,
    CFG_SOG0_MIN_RETURN_RUNTIME,
    CFG_SOG0_LOADSHED_CONFIG,
    CFG_SOG0_LOADSHED_RUNTIME,
    CFG_SOG0_LOADSHED_TIME_ON_BAT,
    /* SOG1 */
    CFG_SOG1_TURN_OFF_DELAY,
    CFG_SOG1_TURN_ON_DELAY,
    CFG_SOG1_MIN_RETURN_RUNTIME,
    CFG_SOG1_LOADSHED_CONFIG,
    CFG_SOG1_LOADSHED_RUNTIME,
    CFG_SOG1_LOADSHED_TIME_ON_BAT,
    /* SOG2 */
    CFG_SOG2_TURN_OFF_DELAY,
    CFG_SOG2_TURN_ON_DELAY,
    CFG_SOG2_MIN_RETURN_RUNTIME,
    CFG_SOG2_LOADSHED_CONFIG,
    CFG_SOG2_LOADSHED_RUNTIME,
    CFG_SOG2_LOADSHED_TIME_ON_BAT,

    /* === Comprehensive register dump descriptors (Phase 3) === */

    /* Status block (regs 0-26) — DIAGNOSTIC */
    CFG_UPS_STATUS,
    CFG_TRANSFER_REASON,
    CFG_MOG_STATUS,
    CFG_SOG0_STATUS,
    CFG_SOG1_STATUS,
    CFG_SOG2_STATUS,
    CFG_SIGNALING_STATUS,
    CFG_GENERAL_ERROR,
    CFG_POWER_SYSTEM_ERROR,
    CFG_BATTERY_SYSTEM_ERROR,
    CFG_REPLACE_BATTERY_TEST_STATUS,
    CFG_RUNTIME_CALIBRATION_STATUS,
    CFG_BATTERY_LIFETIME_STATUS,
    CFG_UI_STATUS,

    /* Dynamic block (regs 128-171) — MEASUREMENT */
    CFG_BATTERY_RUNTIME,
    CFG_BATTERY_CHARGE_PCT,
    CFG_BATTERY_VOLTAGE_POS,
    CFG_BATTERY_VOLTAGE_NEG,
    CFG_BATTERY_INTERNAL_DATE,
    CFG_BATTERY_TEMPERATURE,
    CFG_OUTPUT_LOAD_PCT,
    CFG_OUTPUT_APPARENT_POWER_PCT,
    CFG_OUTPUT_CURRENT,
    CFG_OUTPUT_VOLTAGE,
    CFG_OUTPUT_FREQUENCY,
    CFG_OUTPUT_ENERGY,
    CFG_BYPASS_INPUT_STATUS,
    CFG_BYPASS_VOLTAGE,
    CFG_BYPASS_FREQUENCY,
    CFG_INPUT_STATUS,
    CFG_INPUT_VOLTAGE,
    CFG_EFFICIENCY,
    CFG_MOG_OFF_COUNTDOWN,
    CFG_MOG_ON_COUNTDOWN,
    CFG_MOG_STAYOFF_COUNTDOWN,
    CFG_SOG0_OFF_COUNTDOWN,
    CFG_SOG0_ON_COUNTDOWN,
    CFG_SOG0_STAYOFF_COUNTDOWN,
    CFG_SOG1_OFF_COUNTDOWN,
    CFG_SOG1_ON_COUNTDOWN,
    CFG_SOG1_STAYOFF_COUNTDOWN,
    CFG_SOG2_OFF_COUNTDOWN,
    CFG_SOG2_ON_COUNTDOWN,
    CFG_SOG2_STAYOFF_COUNTDOWN,

    /* Inventory block (regs 516-595) — IDENTITY */
    CFG_FIRMWARE_VERSION,
    CFG_MODEL_NAME,
    CFG_SKU,
    CFG_SERIAL_NUMBER,
    CFG_BATTERY_SKU,
    CFG_NOMINAL_APPARENT_POWER,
    CFG_NOMINAL_REAL_POWER,
    CFG_SOG_RELAY_CONFIG,

    /* Names block (regs 596-635) — CONFIG, writable */
    CFG_UPS_NAME,
    CFG_MOG_NAME,
    CFG_SOG0_NAME,
    CFG_SOG1_NAME,
    CFG_SOG2_NAME,

    CFG_COUNT
};

/* BatteryTestIntervalSetting_BF: AN-176 defines six values (bits 0..5).
 * SMT firmware (verified on SMT1500RM2UC FW UPS 04.1) rejects bits 2
 * (OnStartUpPlus7 = value 4) and 3 (OnStartUpPlus14 = value 8) with
 * Modbus exception 0x04 — same constraint as the SRT line. The bits
 * are spec-defined but apparently never wired into the operational
 * firmware. Strict-on validation in the registry blocks writes of
 * these values before they hit the wire.
 *
 * Do not "fix" the gaps by re-adding 4/8 here without verifying on the
 * specific firmware in question. See docs/reference/apc-smt-modbus.md
 * "Battery test interval — register 1024" for reproduction details. */
static const ups_bitfield_opt_t smt_bat_test_opts[] = {
    { 1,  "never",         "Never" },
    { 2,  "onstart_only",  "Startup Only" },
    { 16, "onstart_7d",    "Startup + 7 Days Since Test" },
    { 32, "onstart_14d",   "Startup + 14 Days Since Test" },
};

/* SensitivitySetting_BF: AN-176 defines three values, all verified
 * accepted on SMT1500RM2UC FW UPS 04.1. */
static const ups_bitfield_opt_t smt_sensitivity_opts[] = {
    { 1, "normal",  "Normal (minimum line deviations to load)" },
    { 2, "reduced", "Reduced (more line deviations to load)" },
    { 4, "low",     "Low (maximum line deviations to load)" },
};

static const ups_bitfield_opt_t smt_voltage_opts[] = {
    { 1,    "vac100", "100 VAC" },
    { 2,    "vac120", "120 VAC" },
    { 4,    "vac200", "200 VAC" },
    { 8,    "vac208", "208 VAC" },
    { 16,   "vac220", "220 VAC" },
    { 32,   "vac230", "230 VAC" },
    { 64,   "vac240", "240 VAC" },
    { 2048, "vac110", "110 VAC" },
};

/* --- FLAGS / BITFIELD options arrays for the comprehensive register dump.
 * Bit definitions transcribed from docs/reference/apc-smt-modbus.md (verified
 * against APC SMT1500RM2UC, FW UPS 04.1). Strict=0 throughout — these are
 * read-only diagnostics, not validation targets.
 *
 * SMT-specific differences from the SRT bit catalog:
 *   - ups_status: bits 10/19/20/21 are *declared* in AN-176 but never
 *     toggled by SMT firmware. Kept here so the bit catalog reflects
 *     the doc; the active_flags renderer just won't ever emit them.
 *   - general_error: bits 6, 9-15 are SRT/SURTD-only — omitted here.
 *   - power_system_error: bit 3 (TransformerDCImbalance), bit 9
 *     (BypassRelay), bit 12 (DCBusOvervoltage), bit 14 (OverCurrent)
 *     are SRT-only — omitted here.
 *   - outlet_status: SMT bit set differs from SRT (no PendingLoadShed
 *     bit 7, no PendingOnDelay bit 8/9 in the SRT shape; instead has
 *     MemberGroupProcess1/2 at bits 10/11 and LowRuntime at bit 12).
 *   - input_status: full bit catalog kept identical to SRT — used by
 *     both reg 150 and reg 147 even though 147 is sentinel on SMT. */

static const ups_bitfield_opt_t smt_ups_status_opts[] = {
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

static const ups_bitfield_opt_t smt_transfer_reason_opts[] = {
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

/* SMT outlet status bits per docs/reference/apc-smt-modbus.md table at line
 * 140-152 — differs from the SRT layout. */
static const ups_bitfield_opt_t smt_outlet_status_opts[] = {
    { 0x00000001, "state_on",                "State: ON" },
    { 0x00000002, "state_off",               "State: OFF" },
    { 0x00000004, "processing_reboot",       "Processing Reboot" },
    { 0x00000008, "processing_shutdown",     "Processing Shutdown" },
    { 0x00000010, "processing_sleep",        "Processing Sleep" },
    { 0x00000080, "pending_off_delay",       "Pending Off Delay" },
    { 0x00000100, "pending_on_ac_presence",  "Pending On AC Presence" },
    { 0x00000200, "pending_on_min_runtime",  "Pending On Min Runtime" },
    { 0x00000400, "member_group_process_1",  "Member Group Process 1" },
    { 0x00000800, "member_group_process_2",  "Member Group Process 2" },
    { 0x00001000, "low_runtime",             "Low Runtime" },
};

static const ups_bitfield_opt_t smt_signaling_status_opts[] = {
    { 0x0001, "power_failure",     "Power Failure" },
    { 0x0002, "shutdown_imminent", "Shutdown Imminent" },
};

/* SMT general_error: bits 0-5, 7, 8 only per docs/reference/apc-smt-modbus.md
 * line 169-179. Bits 6 and 9-15 are SRT/SURTD-only and omitted here. */
static const ups_bitfield_opt_t smt_general_error_opts[] = {
    { 0x0001, "site_wiring",            "Site Wiring" },
    { 0x0002, "eeprom",                 "EEPROM" },
    { 0x0004, "ad_converter",           "A/D Converter" },
    { 0x0008, "logic_power_supply",     "Logic Power Supply" },
    { 0x0010, "internal_communication", "Internal Communication" },
    { 0x0020, "ui_button",              "UI Button" },
    { 0x0080, "epo_active",             "EPO Active" },
    { 0x0100, "firmware_mismatch",      "Firmware Mismatch" },
};

/* SMT power_system_error: SRT-only bits omitted (bit 3 TransformerDCImbalance,
 * bit 9 BypassRelay, bit 12 DCBusOvervoltage, bit 14 OverCurrent). */
static const ups_bitfield_opt_t smt_power_system_error_opts[] = {
    { 0x00000001, "output_overload",      "Output Overload" },
    { 0x00000002, "output_short_circuit", "Output Short Circuit" },
    { 0x00000004, "output_overvoltage",   "Output Overvoltage" },
    { 0x00000010, "overtemperature",      "Overtemperature" },
    { 0x00000020, "backfeed_relay",       "Backfeed Relay" },
    { 0x00000040, "avr_relay",            "AVR Relay" },
    { 0x00000080, "pfc_input_relay",      "PFC Input Relay" },
    { 0x00000100, "output_relay",         "Output Relay" },
    { 0x00000400, "fan",                  "Fan" },
    { 0x00000800, "pfc",                  "PFC" },
    { 0x00002000, "inverter",             "Inverter" },
};

static const ups_bitfield_opt_t smt_battery_system_error_opts[] = {
    { 0x0001, "disconnected",       "Disconnected" },
    { 0x0002, "overvoltage",        "Overvoltage" },
    { 0x0004, "needs_replacement",  "Needs Replacement" },
    { 0x0008, "overtemp_critical",  "Overtemperature Critical" },
    { 0x0010, "charger",            "Charger" },
    { 0x0020, "temperature_sensor", "Temperature Sensor" },
    { 0x0080, "overtemp_warning",   "Overtemperature Warning" },
    { 0x0100, "general_error",      "General Error" },
    { 0x0200, "communication",      "Communication" },
};

static const ups_bitfield_opt_t smt_replace_battery_test_status_opts[] = {
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

static const ups_bitfield_opt_t smt_runtime_calibration_status_opts[] = {
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

static const ups_bitfield_opt_t smt_battery_lifetime_status_opts[] = {
    { 0x0001, "ok",                    "Lifetime OK" },
    { 0x0002, "near_end",              "Lifetime Near End" },
    { 0x0004, "exceeded",              "Lifetime Exceeded" },
    { 0x0008, "near_end_acknowledged", "Lifetime Near End Acknowledged" },
    { 0x0010, "exceeded_acknowledged", "Lifetime Exceeded Acknowledged" },
};

static const ups_bitfield_opt_t smt_ui_status_opts[] = {
    { 0x0001, "continuous_test_in_progress", "Continuous Test in Progress" },
    { 0x0002, "audible_alarm_in_progress",   "Audible Alarm in Progress" },
    { 0x0004, "audible_alarm_muted",         "Audible Alarm Muted" },
    { 0x0008, "any_button_pressed_recently", "Any Button Pressed Recently" },
};

/* Input status bits — full catalog. Used by both reg 150 (real input)
 * and reg 147 (bypass input, sentinel on SMT). */
static const ups_bitfield_opt_t smt_input_status_opts[] = {
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

static const ups_bitfield_opt_t smt_sog_relay_config_opts[] = {
    { 0x0001, "mog_present",  "MOG Present" },
    { 0x0002, "sog0_present", "SOG0 Present" },
    { 0x0004, "sog1_present", "SOG1 Present" },
    { 0x0008, "sog2_present", "SOG2 Present" },
};

static const ups_config_reg_t smt_config_regs[] = {
    [CFG_TRANSFER_HIGH] = {
        "transfer_high", "Upper Acceptable Input Voltage", "V", "transfer",
        1026, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 110, 150 } },
    [CFG_TRANSFER_LOW] = {
        "transfer_low", "Lower Acceptable Input Voltage", "V", "transfer",
        1027, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 80, 120 } },
    [CFG_BAT_TEST_INTERVAL] = {
        "bat_test_interval", "Battery Test Interval", NULL, "battery",
        1024, 1, UPS_CFG_BITFIELD, 1, 1,
        .meta.bitfield = { smt_bat_test_opts,
                           sizeof(smt_bat_test_opts)/sizeof(smt_bat_test_opts[0]),
                           1 /* strict */ } },
    [CFG_SENSITIVITY] = {
        "sensitivity", "Line Sensitivity", NULL, "input",
        1028, 1, UPS_CFG_BITFIELD, 1, 1,
        .meta.bitfield = { smt_sensitivity_opts,
                           sizeof(smt_sensitivity_opts)/sizeof(smt_sensitivity_opts[0]),
                           1 /* strict */ } },
    [CFG_OUTPUT_VOLTAGE_SETTING] = {
        "output_voltage_setting", "Output Voltage Setting", NULL, "output",
        592, 1, UPS_CFG_BITFIELD, 1, 0,
        .meta.bitfield = { smt_voltage_opts,
                           sizeof(smt_voltage_opts)/sizeof(smt_voltage_opts[0]),
                           1 /* strict */ } },
    [CFG_MANUFACTURE_DATE] = {
        "manufacture_date", "Manufacture Date", "days since 2000-01-01", "info",
        591, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 65535 } },
    [CFG_BATTERY_DATE] = {
        "battery_date", "Battery Install Date", "days since 2000-01-01", "info",
        595, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 65535 } },

    /* MOG outlet timing + load shed */
    [CFG_MOG_TURN_OFF_DELAY] = {
        "mog_turn_off_delay", "MOG Turn Off Countdown", "s", "outlet_delays",
        1029, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    [CFG_MOG_TURN_ON_DELAY] = {
        "mog_turn_on_delay", "MOG Turn On Countdown", "s", "outlet_delays",
        1030, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    [CFG_MOG_MIN_RETURN_RUNTIME] = {
        "mog_min_return_runtime", "MOG Minimum Return Runtime", "s", "outlet_delays",
        1033, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    /* Load Shed registers (1054, 1056, 1064, 1068, 1072, 1073) — declared
     * in 990-9840B as RW for SMX/SMT but not implemented for write on
     * SMT1500RM2UC FW UPS 04.1 (verified by probe: writes to 1064 and
     * 1072 return Modbus exception 0x02 "Illegal Data Address"; reads
     * return AN-176 §3.2.2 "not applicable" sentinels: 0x0000 for the
     * config bitfields, 0xFFFF for the threshold scalars). Likely the
     * whole load-shed feature is gated on a firmware capability bit
     * we don't have on this vintage.
     *
     * Marked writable=0 so the UI offers no Edit button rather than
     * letting the user attempt writes that always fail. Re-verify on
     * FW 18.x — newer firmware may implement load shed, in which case
     * flip these back to writable=1 (and re-evaluate whether 65535
     * functions as a "disabled" sentinel on the threshold registers). */
    [CFG_MOG_LOADSHED_CONFIG] = {
        "mog_loadshed_config", "MOG Load Shed Config", NULL, "load_shed",
        1054, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 65535 } },
    [CFG_MOG_LOADSHED_RUNTIME] = {
        "mog_loadshed_runtime", "MOG Load Shed Runtime Threshold", "s", "load_shed",
        1072, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 32767 } },
    [CFG_MOG_LOADSHED_TIME_ON_BAT] = {
        "mog_loadshed_time_on_bat", "MOG Load Shed Time on Battery", "s", "load_shed",
        1073, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 32767 } },

    /* SOG0 — narrowed away by resolve_config_regs when SOG0 absent */
    [CFG_SOG0_TURN_OFF_DELAY] = {
        "sog0_turn_off_delay", "SOG0 Turn Off Countdown", "s", "outlet_delays",
        1034, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    [CFG_SOG0_TURN_ON_DELAY] = {
        "sog0_turn_on_delay", "SOG0 Turn On Countdown", "s", "outlet_delays",
        1035, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    [CFG_SOG0_MIN_RETURN_RUNTIME] = {
        "sog0_min_return_runtime", "SOG0 Minimum Return Runtime", "s", "outlet_delays",
        1038, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    /* SOG0 Load Shed — same firmware-gap as MOG above; see comment near
     * CFG_MOG_LOADSHED_CONFIG. */
    [CFG_SOG0_LOADSHED_CONFIG] = {
        "sog0_loadshed_config", "SOG0 Load Shed Config", NULL, "load_shed",
        1056, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 65535 } },
    [CFG_SOG0_LOADSHED_RUNTIME] = {
        "sog0_loadshed_runtime", "SOG0 Load Shed Runtime Threshold", "s", "load_shed",
        1064, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 32767 } },
    [CFG_SOG0_LOADSHED_TIME_ON_BAT] = {
        "sog0_loadshed_time_on_bat", "SOG0 Load Shed Time on Battery", "s", "load_shed",
        1068, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 32767 } },

    /* SOG1 */
    [CFG_SOG1_TURN_OFF_DELAY] = {
        "sog1_turn_off_delay", "SOG1 Turn Off Countdown", "s", "outlet_delays",
        1039, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    [CFG_SOG1_TURN_ON_DELAY] = {
        "sog1_turn_on_delay", "SOG1 Turn On Countdown", "s", "outlet_delays",
        1040, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    [CFG_SOG1_MIN_RETURN_RUNTIME] = {
        "sog1_min_return_runtime", "SOG1 Minimum Return Runtime", "s", "outlet_delays",
        1043, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    /* SOG1 Load Shed — same firmware-gap as MOG / SOG0; see comment near
     * CFG_MOG_LOADSHED_CONFIG. (resolve_config_regs drops these on SKUs
     * without SOG1 anyway.) */
    [CFG_SOG1_LOADSHED_CONFIG] = {
        "sog1_loadshed_config", "SOG1 Load Shed Config", NULL, "load_shed",
        1058, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 65535 } },
    [CFG_SOG1_LOADSHED_RUNTIME] = {
        "sog1_loadshed_runtime", "SOG1 Load Shed Runtime Threshold", "s", "load_shed",
        1065, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 32767 } },
    [CFG_SOG1_LOADSHED_TIME_ON_BAT] = {
        "sog1_loadshed_time_on_bat", "SOG1 Load Shed Time on Battery", "s", "load_shed",
        1069, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 32767 } },

    /* SOG2 */
    [CFG_SOG2_TURN_OFF_DELAY] = {
        "sog2_turn_off_delay", "SOG2 Turn Off Countdown", "s", "outlet_delays",
        1044, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    [CFG_SOG2_TURN_ON_DELAY] = {
        "sog2_turn_on_delay", "SOG2 Turn On Countdown", "s", "outlet_delays",
        1045, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    [CFG_SOG2_MIN_RETURN_RUNTIME] = {
        "sog2_min_return_runtime", "SOG2 Minimum Return Runtime", "s", "outlet_delays",
        1048, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    /* SOG2 Load Shed — same firmware-gap as the other groups. */
    [CFG_SOG2_LOADSHED_CONFIG] = {
        "sog2_loadshed_config", "SOG2 Load Shed Config", NULL, "load_shed",
        1060, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 65535 } },
    [CFG_SOG2_LOADSHED_RUNTIME] = {
        "sog2_loadshed_runtime", "SOG2 Load Shed Runtime Threshold", "s", "load_shed",
        1066, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 32767 } },
    [CFG_SOG2_LOADSHED_TIME_ON_BAT] = {
        "sog2_loadshed_time_on_bat", "SOG2 Load Shed Time on Battery", "s", "load_shed",
        1070, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 32767 } },

    /* === Comprehensive register dump descriptors (Phase 3) ===
     * Added 2026-04-25 to surface every documented Modbus register on
     * /api/about. Coverage is per docs/reference/apc-smt-modbus.md. Existing
     * entries above (transfer_*, bat_test_interval, sensitivity, outlet
     * timings, load_shed*, output_voltage_setting, dates) cover the
     * operator-tunable settings; the entries below cover the read-only
     * diagnostic / measurement / identity surface plus the writable
     * Names block.
     *
     * SMT-specific sentinels (per docs/reference/apc-smt-modbus.md):
     *   - reg 132 (Battery.Negative.VoltageDC) — 0xFFFF
     *   - reg 147 (Bypass.InputStatus_BF) — 0x0000
     *   - reg 148/149 (Bypass voltage/freq) — 0xFFFF
     *   - reg 151 (Input.VoltageAC) — 0xFFFF when unmeasurable
     *   - reg 154 (Efficiency_EN) — 0xFFFF as N/A
     *   - regs 155-170 (outlet countdowns) — 0xFFFF (int16) /
     *     0xFFFFFFFF (int32) means "no countdown active"
     *
     * SRT-only registers omitted: reg 580-587 (External Battery SKU),
     * reg 593 (AcceptableFrequencySetting_BF). */

    /* Status block (registers 0-26) — DIAGNOSTIC */
    [CFG_UPS_STATUS] = {
        "ups_status", "UPS Status", NULL, "status",
        0, 2, UPS_CFG_FLAGS, 1, 0,
        .meta.bitfield = { smt_ups_status_opts,
                           sizeof(smt_ups_status_opts)/sizeof(smt_ups_status_opts[0]),
                           0 },
        .category = UPS_REG_CATEGORY_DIAGNOSTIC },
    [CFG_TRANSFER_REASON] = {
        "transfer_reason", "Last Transfer Reason", NULL, "status",
        2, 1, UPS_CFG_BITFIELD, 1, 0,
        .meta.bitfield = { smt_transfer_reason_opts,
                           sizeof(smt_transfer_reason_opts)/sizeof(smt_transfer_reason_opts[0]),
                           0 },
        .category = UPS_REG_CATEGORY_DIAGNOSTIC },
    [CFG_MOG_STATUS] = {
        "mog_status", "MOG Status", NULL, "status",
        3, 2, UPS_CFG_FLAGS, 1, 0,
        .meta.bitfield = { smt_outlet_status_opts,
                           sizeof(smt_outlet_status_opts)/sizeof(smt_outlet_status_opts[0]),
                           0 },
        .category = UPS_REG_CATEGORY_DIAGNOSTIC },
    [CFG_SOG0_STATUS] = {
        "sog0_status", "SOG0 Status", NULL, "status",
        6, 2, UPS_CFG_FLAGS, 1, 0,
        .meta.bitfield = { smt_outlet_status_opts,
                           sizeof(smt_outlet_status_opts)/sizeof(smt_outlet_status_opts[0]),
                           0 },
        .category = UPS_REG_CATEGORY_DIAGNOSTIC },
    [CFG_SOG1_STATUS] = {
        "sog1_status", "SOG1 Status", NULL, "status",
        9, 2, UPS_CFG_FLAGS, 1, 0,
        .meta.bitfield = { smt_outlet_status_opts,
                           sizeof(smt_outlet_status_opts)/sizeof(smt_outlet_status_opts[0]),
                           0 },
        .category = UPS_REG_CATEGORY_DIAGNOSTIC },
    [CFG_SOG2_STATUS] = {
        "sog2_status", "SOG2 Status", NULL, "status",
        12, 2, UPS_CFG_FLAGS, 1, 0,
        .meta.bitfield = { smt_outlet_status_opts,
                           sizeof(smt_outlet_status_opts)/sizeof(smt_outlet_status_opts[0]),
                           0 },
        .category = UPS_REG_CATEGORY_DIAGNOSTIC },
    [CFG_SIGNALING_STATUS] = {
        "signaling_status", "Simple Signaling Status", NULL, "status",
        18, 1, UPS_CFG_FLAGS, 1, 0,
        .meta.bitfield = { smt_signaling_status_opts,
                           sizeof(smt_signaling_status_opts)/sizeof(smt_signaling_status_opts[0]),
                           0 },
        .category = UPS_REG_CATEGORY_DIAGNOSTIC },
    [CFG_GENERAL_ERROR] = {
        "general_error", "General Error", NULL, "errors",
        19, 1, UPS_CFG_FLAGS, 1, 0,
        .meta.bitfield = { smt_general_error_opts,
                           sizeof(smt_general_error_opts)/sizeof(smt_general_error_opts[0]),
                           0 },
        .category = UPS_REG_CATEGORY_DIAGNOSTIC },
    [CFG_POWER_SYSTEM_ERROR] = {
        "power_system_error", "Power System Error", NULL, "errors",
        20, 2, UPS_CFG_FLAGS, 1, 0,
        .meta.bitfield = { smt_power_system_error_opts,
                           sizeof(smt_power_system_error_opts)/sizeof(smt_power_system_error_opts[0]),
                           0 },
        .category = UPS_REG_CATEGORY_DIAGNOSTIC },
    [CFG_BATTERY_SYSTEM_ERROR] = {
        "battery_system_error", "Battery System Error", NULL, "errors",
        22, 1, UPS_CFG_FLAGS, 1, 0,
        .meta.bitfield = { smt_battery_system_error_opts,
                           sizeof(smt_battery_system_error_opts)/sizeof(smt_battery_system_error_opts[0]),
                           0 },
        .category = UPS_REG_CATEGORY_DIAGNOSTIC },
    [CFG_REPLACE_BATTERY_TEST_STATUS] = {
        "replace_battery_test_status", "Replace Battery Test Status", NULL, "status",
        23, 1, UPS_CFG_FLAGS, 1, 0,
        .meta.bitfield = { smt_replace_battery_test_status_opts,
                           sizeof(smt_replace_battery_test_status_opts)/sizeof(smt_replace_battery_test_status_opts[0]),
                           0 },
        .category = UPS_REG_CATEGORY_DIAGNOSTIC },
    [CFG_RUNTIME_CALIBRATION_STATUS] = {
        "runtime_calibration_status", "Runtime Calibration Status", NULL, "status",
        24, 1, UPS_CFG_FLAGS, 1, 0,
        .meta.bitfield = { smt_runtime_calibration_status_opts,
                           sizeof(smt_runtime_calibration_status_opts)/sizeof(smt_runtime_calibration_status_opts[0]),
                           0 },
        .category = UPS_REG_CATEGORY_DIAGNOSTIC },
    [CFG_BATTERY_LIFETIME_STATUS] = {
        "battery_lifetime_status", "Battery Lifetime Status", NULL, "status",
        25, 1, UPS_CFG_FLAGS, 1, 0,
        .meta.bitfield = { smt_battery_lifetime_status_opts,
                           sizeof(smt_battery_lifetime_status_opts)/sizeof(smt_battery_lifetime_status_opts[0]),
                           0 },
        .category = UPS_REG_CATEGORY_DIAGNOSTIC },
    [CFG_UI_STATUS] = {
        "ui_status", "UI Status", NULL, "status",
        26, 1, UPS_CFG_FLAGS, 1, 0,
        .meta.bitfield = { smt_ui_status_opts,
                           sizeof(smt_ui_status_opts)/sizeof(smt_ui_status_opts[0]),
                           0 },
        .category = UPS_REG_CATEGORY_DIAGNOSTIC },

    /* Dynamic block (registers 128-171) — MEASUREMENT */
    [CFG_BATTERY_RUNTIME] = {
        "battery_runtime", "Battery Runtime", "s", "battery",
        128, 2, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 0, 0 },
        .category = UPS_REG_CATEGORY_MEASUREMENT },
    [CFG_BATTERY_CHARGE_PCT] = {
        "battery_charge_pct", "Battery Charge", "%", "battery",
        130, 1, UPS_CFG_SCALAR, 512, 0, .meta.scalar = { 0, 0, 0 },
        .category = UPS_REG_CATEGORY_MEASUREMENT },
    [CFG_BATTERY_VOLTAGE_POS] = {
        "battery_voltage_pos", "Battery Positive Voltage", "V", "battery",
        131, 1, UPS_CFG_SCALAR, 32, 0, .meta.scalar = { 0, 0, 0 },
        .category = UPS_REG_CATEGORY_MEASUREMENT },
    [CFG_BATTERY_VOLTAGE_NEG] = {
        "battery_voltage_neg", "Battery Negative Voltage", "V", "battery",
        132, 1, UPS_CFG_SCALAR, 32, 0, .meta.scalar = { 0, 0, 0 },
        .category = UPS_REG_CATEGORY_MEASUREMENT,
        .has_sentinel = 1, .sentinel_value = 0xFFFF },
    [CFG_BATTERY_INTERNAL_DATE] = {
        "battery_internal_date", "Battery Internal Date", "days since 2000-01-01", "battery",
        133, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 0, 0 },
        .category = UPS_REG_CATEGORY_MEASUREMENT },
    [CFG_BATTERY_TEMPERATURE] = {
        "battery_temperature", "Battery Temperature", "\xC2\xB0""C", "battery",
        135, 1, UPS_CFG_SCALAR, 128, 0, .meta.scalar = { 0, 0, 0 },
        .category = UPS_REG_CATEGORY_MEASUREMENT },
    [CFG_OUTPUT_LOAD_PCT] = {
        "output_load_pct", "Output Load", "%", "output",
        136, 1, UPS_CFG_SCALAR, 256, 0, .meta.scalar = { 0, 0, 0 },
        .category = UPS_REG_CATEGORY_MEASUREMENT },
    [CFG_OUTPUT_APPARENT_POWER_PCT] = {
        "output_apparent_power_pct", "Output Apparent Power", "%", "output",
        138, 1, UPS_CFG_SCALAR, 256, 0, .meta.scalar = { 0, 0, 0 },
        .category = UPS_REG_CATEGORY_MEASUREMENT },
    [CFG_OUTPUT_CURRENT] = {
        "output_current", "Output Current", "A", "output",
        140, 1, UPS_CFG_SCALAR, 32, 0, .meta.scalar = { 0, 0, 0 },
        .category = UPS_REG_CATEGORY_MEASUREMENT },
    [CFG_OUTPUT_VOLTAGE] = {
        "output_voltage", "Output Voltage", "V", "output",
        142, 1, UPS_CFG_SCALAR, 64, 0, .meta.scalar = { 0, 0, 0 },
        .category = UPS_REG_CATEGORY_MEASUREMENT },
    [CFG_OUTPUT_FREQUENCY] = {
        "output_frequency", "Output Frequency", "Hz", "output",
        144, 1, UPS_CFG_SCALAR, 128, 0, .meta.scalar = { 0, 0, 0 },
        .category = UPS_REG_CATEGORY_MEASUREMENT },
    [CFG_OUTPUT_ENERGY] = {
        "output_energy", "Output Energy", "Wh", "output",
        145, 2, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 0, 0 },
        .category = UPS_REG_CATEGORY_MEASUREMENT },
    [CFG_BYPASS_INPUT_STATUS] = {
        "bypass_input_status", "Bypass Input Status", NULL, "bypass",
        147, 1, UPS_CFG_FLAGS, 1, 0,
        .meta.bitfield = { smt_input_status_opts,
                           sizeof(smt_input_status_opts)/sizeof(smt_input_status_opts[0]),
                           0 },
        .category = UPS_REG_CATEGORY_MEASUREMENT,
        .has_sentinel = 1, .sentinel_value = 0x0000 },
    [CFG_BYPASS_VOLTAGE] = {
        "bypass_voltage", "Bypass Voltage", "V", "bypass",
        148, 1, UPS_CFG_SCALAR, 64, 0, .meta.scalar = { 0, 0, 0 },
        .category = UPS_REG_CATEGORY_MEASUREMENT,
        .has_sentinel = 1, .sentinel_value = 0xFFFF },
    [CFG_BYPASS_FREQUENCY] = {
        "bypass_frequency", "Bypass Frequency", "Hz", "bypass",
        149, 1, UPS_CFG_SCALAR, 128, 0, .meta.scalar = { 0, 0, 0 },
        .category = UPS_REG_CATEGORY_MEASUREMENT,
        .has_sentinel = 1, .sentinel_value = 0xFFFF },
    [CFG_INPUT_STATUS] = {
        "input_status", "Input Status", NULL, "input",
        150, 1, UPS_CFG_FLAGS, 1, 0,
        .meta.bitfield = { smt_input_status_opts,
                           sizeof(smt_input_status_opts)/sizeof(smt_input_status_opts[0]),
                           0 },
        .category = UPS_REG_CATEGORY_MEASUREMENT },
    [CFG_INPUT_VOLTAGE] = {
        "input_voltage", "Input Voltage", "V", "input",
        151, 1, UPS_CFG_SCALAR, 64, 0, .meta.scalar = { 0, 0, 0 },
        .category = UPS_REG_CATEGORY_MEASUREMENT,
        .has_sentinel = 1, .sentinel_value = 0xFFFF },
    [CFG_EFFICIENCY] = {
        "efficiency", "Efficiency", "%", "ups",
        154, 1, UPS_CFG_SCALAR, 128, 0, .meta.scalar = { 0, 0, 0 },
        .category = UPS_REG_CATEGORY_MEASUREMENT,
        .has_sentinel = 1, .sentinel_value = 0xFFFF },
    [CFG_MOG_OFF_COUNTDOWN] = {
        "mog_off_countdown", "MOG Turn Off Countdown", "s", "outlet_countdowns",
        155, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 0, 0 },
        .category = UPS_REG_CATEGORY_MEASUREMENT,
        .has_sentinel = 1, .sentinel_value = 0xFFFF },
    [CFG_MOG_ON_COUNTDOWN] = {
        "mog_on_countdown", "MOG Turn On Countdown", "s", "outlet_countdowns",
        156, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 0, 0 },
        .category = UPS_REG_CATEGORY_MEASUREMENT,
        .has_sentinel = 1, .sentinel_value = 0xFFFF },
    [CFG_MOG_STAYOFF_COUNTDOWN] = {
        "mog_stayoff_countdown", "MOG Stay Off Countdown", "s", "outlet_countdowns",
        157, 2, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 0, 0 },
        .category = UPS_REG_CATEGORY_MEASUREMENT,
        .has_sentinel = 1, .sentinel_value = 0xFFFFFFFF },
    [CFG_SOG0_OFF_COUNTDOWN] = {
        "sog0_off_countdown", "SOG0 Turn Off Countdown", "s", "outlet_countdowns",
        159, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 0, 0 },
        .category = UPS_REG_CATEGORY_MEASUREMENT,
        .has_sentinel = 1, .sentinel_value = 0xFFFF },
    [CFG_SOG0_ON_COUNTDOWN] = {
        "sog0_on_countdown", "SOG0 Turn On Countdown", "s", "outlet_countdowns",
        160, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 0, 0 },
        .category = UPS_REG_CATEGORY_MEASUREMENT,
        .has_sentinel = 1, .sentinel_value = 0xFFFF },
    [CFG_SOG0_STAYOFF_COUNTDOWN] = {
        "sog0_stayoff_countdown", "SOG0 Stay Off Countdown", "s", "outlet_countdowns",
        161, 2, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 0, 0 },
        .category = UPS_REG_CATEGORY_MEASUREMENT,
        .has_sentinel = 1, .sentinel_value = 0xFFFFFFFF },
    [CFG_SOG1_OFF_COUNTDOWN] = {
        "sog1_off_countdown", "SOG1 Turn Off Countdown", "s", "outlet_countdowns",
        163, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 0, 0 },
        .category = UPS_REG_CATEGORY_MEASUREMENT,
        .has_sentinel = 1, .sentinel_value = 0xFFFF },
    [CFG_SOG1_ON_COUNTDOWN] = {
        "sog1_on_countdown", "SOG1 Turn On Countdown", "s", "outlet_countdowns",
        164, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 0, 0 },
        .category = UPS_REG_CATEGORY_MEASUREMENT,
        .has_sentinel = 1, .sentinel_value = 0xFFFF },
    [CFG_SOG1_STAYOFF_COUNTDOWN] = {
        "sog1_stayoff_countdown", "SOG1 Stay Off Countdown", "s", "outlet_countdowns",
        165, 2, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 0, 0 },
        .category = UPS_REG_CATEGORY_MEASUREMENT,
        .has_sentinel = 1, .sentinel_value = 0xFFFFFFFF },
    [CFG_SOG2_OFF_COUNTDOWN] = {
        "sog2_off_countdown", "SOG2 Turn Off Countdown", "s", "outlet_countdowns",
        167, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 0, 0 },
        .category = UPS_REG_CATEGORY_MEASUREMENT,
        .has_sentinel = 1, .sentinel_value = 0xFFFF },
    [CFG_SOG2_ON_COUNTDOWN] = {
        "sog2_on_countdown", "SOG2 Turn On Countdown", "s", "outlet_countdowns",
        168, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 0, 0 },
        .category = UPS_REG_CATEGORY_MEASUREMENT,
        .has_sentinel = 1, .sentinel_value = 0xFFFF },
    [CFG_SOG2_STAYOFF_COUNTDOWN] = {
        "sog2_stayoff_countdown", "SOG2 Stay Off Countdown", "s", "outlet_countdowns",
        169, 2, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 0, 0 },
        .category = UPS_REG_CATEGORY_MEASUREMENT,
        .has_sentinel = 1, .sentinel_value = 0xFFFFFFFF },

    /* Inventory block (registers 516-595) — IDENTITY.
     * No external_battery_sku descriptor: regs 580-587 are SRT-only.
     * No freq_tolerance descriptor: reg 593 reads 0x0000 on SMT.
     * manufacture_date (reg 591) is in the operator config block above. */
    [CFG_FIRMWARE_VERSION] = {
        "firmware_version", "Firmware Version", NULL, "identity",
        516, 8, UPS_CFG_STRING, 1, 0, .meta.string = { .max_chars = 16 },
        .category = UPS_REG_CATEGORY_IDENTITY },
    [CFG_MODEL_NAME] = {
        "model_name", "Model Name", NULL, "identity",
        532, 16, UPS_CFG_STRING, 1, 0, .meta.string = { .max_chars = 32 },
        .category = UPS_REG_CATEGORY_IDENTITY },
    [CFG_SKU] = {
        "sku", "SKU", NULL, "identity",
        548, 16, UPS_CFG_STRING, 1, 0, .meta.string = { .max_chars = 32 },
        .category = UPS_REG_CATEGORY_IDENTITY },
    [CFG_SERIAL_NUMBER] = {
        "serial_number", "Serial Number", NULL, "identity",
        564, 8, UPS_CFG_STRING, 1, 0, .meta.string = { .max_chars = 16 },
        .category = UPS_REG_CATEGORY_IDENTITY },
    [CFG_BATTERY_SKU] = {
        "battery_sku", "Battery SKU", NULL, "identity",
        572, 8, UPS_CFG_STRING, 1, 0, .meta.string = { .max_chars = 16 },
        .category = UPS_REG_CATEGORY_IDENTITY },
    [CFG_NOMINAL_APPARENT_POWER] = {
        "nominal_apparent_power", "Nominal Apparent Power", "VA", "identity",
        588, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 0, 0 },
        .category = UPS_REG_CATEGORY_IDENTITY },
    [CFG_NOMINAL_REAL_POWER] = {
        "nominal_real_power", "Nominal Real Power", "W", "identity",
        589, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 0, 0 },
        .category = UPS_REG_CATEGORY_IDENTITY },
    [CFG_SOG_RELAY_CONFIG] = {
        "sog_relay_config", "Outlet Group Relay Configuration", NULL, "identity",
        590, 1, UPS_CFG_FLAGS, 1, 0,
        .meta.bitfield = { smt_sog_relay_config_opts,
                           sizeof(smt_sog_relay_config_opts)/sizeof(smt_sog_relay_config_opts[0]),
                           0 },
        .category = UPS_REG_CATEGORY_IDENTITY },

    /* Names block (registers 596-635) — CONFIG, writable */
    [CFG_UPS_NAME] = {
        "ups_name", "UPS Name", NULL, "names",
        596, 8, UPS_CFG_STRING, 1, 1, .meta.string = { .max_chars = 16 } },
    [CFG_MOG_NAME] = {
        "mog_name", "MOG Name", NULL, "names",
        604, 8, UPS_CFG_STRING, 1, 1, .meta.string = { .max_chars = 16 } },
    [CFG_SOG0_NAME] = {
        "sog0_name", "SOG0 Name", NULL, "names",
        612, 8, UPS_CFG_STRING, 1, 1, .meta.string = { .max_chars = 16 } },
    [CFG_SOG1_NAME] = {
        "sog1_name", "SOG1 Name", NULL, "names",
        620, 8, UPS_CFG_STRING, 1, 1, .meta.string = { .max_chars = 16 } },
    [CFG_SOG2_NAME] = {
        "sog2_name", "SOG2 Name", NULL, "names",
        628, 8, UPS_CFG_STRING, 1, 1, .meta.string = { .max_chars = 16 } },
};

_Static_assert(sizeof(smt_config_regs) / sizeof(smt_config_regs[0]) == CFG_COUNT,
               "smt_config_regs[] size out of sync with CFG_COUNT");

/* Read SOGRelayConfigSetting_BF (reg 590) and drop SOG-specific
 * descriptors for groups that aren't physically present. The MOG and
 * non-outlet descriptors are always kept. If the read fails we fall
 * back to "everything is present" so the user can still see the full
 * surface — better an over-eager UI than a silently empty one. */
static size_t smt_resolve_config_regs(void *transport,
                                      const ups_config_reg_t *default_regs,
                                      size_t default_count,
                                      ups_config_reg_t *out)
{
    (void)default_count;

    uint16_t sog_bits = 0xFFFF;  /* fallback: assume all present */
    uint16_t r;
    if (modbus_read_registers(mb(transport), SMT_REG_SOG_CONFIG, 1, &r) == 1)
        sog_bits = r;

    int has_sog0 = (sog_bits & SMT_SOG_HAS_SOG0) != 0;
    int has_sog1 = (sog_bits & SMT_SOG_HAS_SOG1) != 0;
    int has_sog2 = (sog_bits & SMT_SOG_HAS_SOG2) != 0;

    size_t n = 0;
    #define KEEP(idx)         do { out[n++] = default_regs[(idx)]; } while (0)
    #define KEEP_IF(idx, cnd) do { if (cnd) out[n++] = default_regs[(idx)]; } while (0)

    /* Always-present descriptors */
    KEEP(CFG_TRANSFER_HIGH);
    KEEP(CFG_TRANSFER_LOW);
    KEEP(CFG_BAT_TEST_INTERVAL);
    KEEP(CFG_SENSITIVITY);
    KEEP(CFG_OUTPUT_VOLTAGE_SETTING);
    KEEP(CFG_MANUFACTURE_DATE);
    KEEP(CFG_BATTERY_DATE);
    KEEP(CFG_MOG_TURN_OFF_DELAY);
    KEEP(CFG_MOG_TURN_ON_DELAY);
    KEEP(CFG_MOG_MIN_RETURN_RUNTIME);
    KEEP(CFG_MOG_LOADSHED_CONFIG);
    KEEP(CFG_MOG_LOADSHED_RUNTIME);
    KEEP(CFG_MOG_LOADSHED_TIME_ON_BAT);

    KEEP_IF(CFG_SOG0_TURN_OFF_DELAY,         has_sog0);
    KEEP_IF(CFG_SOG0_TURN_ON_DELAY,          has_sog0);
    KEEP_IF(CFG_SOG0_MIN_RETURN_RUNTIME,     has_sog0);
    KEEP_IF(CFG_SOG0_LOADSHED_CONFIG,        has_sog0);
    KEEP_IF(CFG_SOG0_LOADSHED_RUNTIME,       has_sog0);
    KEEP_IF(CFG_SOG0_LOADSHED_TIME_ON_BAT,   has_sog0);

    KEEP_IF(CFG_SOG1_TURN_OFF_DELAY,         has_sog1);
    KEEP_IF(CFG_SOG1_TURN_ON_DELAY,          has_sog1);
    KEEP_IF(CFG_SOG1_MIN_RETURN_RUNTIME,     has_sog1);
    KEEP_IF(CFG_SOG1_LOADSHED_CONFIG,        has_sog1);
    KEEP_IF(CFG_SOG1_LOADSHED_RUNTIME,       has_sog1);
    KEEP_IF(CFG_SOG1_LOADSHED_TIME_ON_BAT,   has_sog1);

    KEEP_IF(CFG_SOG2_TURN_OFF_DELAY,         has_sog2);
    KEEP_IF(CFG_SOG2_TURN_ON_DELAY,          has_sog2);
    KEEP_IF(CFG_SOG2_MIN_RETURN_RUNTIME,     has_sog2);
    KEEP_IF(CFG_SOG2_LOADSHED_CONFIG,        has_sog2);
    KEEP_IF(CFG_SOG2_LOADSHED_RUNTIME,       has_sog2);
    KEEP_IF(CFG_SOG2_LOADSHED_TIME_ON_BAT,   has_sog2);

    /* === Comprehensive register dump descriptors (Phase 3) ===
     * Status / dynamic / inventory / names — same SOG-presence narrowing
     * applies to anything tied to a specific outlet group. Always-present
     * descriptors (status block bitfields, identity, MOG-only, input,
     * battery, output) are kept unconditionally. */

    /* Status block — always present except SOG-specific status registers */
    KEEP(CFG_UPS_STATUS);
    KEEP(CFG_TRANSFER_REASON);
    KEEP(CFG_MOG_STATUS);
    KEEP_IF(CFG_SOG0_STATUS,                 has_sog0);
    KEEP_IF(CFG_SOG1_STATUS,                 has_sog1);
    KEEP_IF(CFG_SOG2_STATUS,                 has_sog2);
    KEEP(CFG_SIGNALING_STATUS);
    KEEP(CFG_GENERAL_ERROR);
    KEEP(CFG_POWER_SYSTEM_ERROR);
    KEEP(CFG_BATTERY_SYSTEM_ERROR);
    KEEP(CFG_REPLACE_BATTERY_TEST_STATUS);
    KEEP(CFG_RUNTIME_CALIBRATION_STATUS);
    KEEP(CFG_BATTERY_LIFETIME_STATUS);
    KEEP(CFG_UI_STATUS);

    /* Dynamic block — always present except SOG-specific countdowns */
    KEEP(CFG_BATTERY_RUNTIME);
    KEEP(CFG_BATTERY_CHARGE_PCT);
    KEEP(CFG_BATTERY_VOLTAGE_POS);
    KEEP(CFG_BATTERY_VOLTAGE_NEG);
    KEEP(CFG_BATTERY_INTERNAL_DATE);
    KEEP(CFG_BATTERY_TEMPERATURE);
    KEEP(CFG_OUTPUT_LOAD_PCT);
    KEEP(CFG_OUTPUT_APPARENT_POWER_PCT);
    KEEP(CFG_OUTPUT_CURRENT);
    KEEP(CFG_OUTPUT_VOLTAGE);
    KEEP(CFG_OUTPUT_FREQUENCY);
    KEEP(CFG_OUTPUT_ENERGY);
    KEEP(CFG_BYPASS_INPUT_STATUS);
    KEEP(CFG_BYPASS_VOLTAGE);
    KEEP(CFG_BYPASS_FREQUENCY);
    KEEP(CFG_INPUT_STATUS);
    KEEP(CFG_INPUT_VOLTAGE);
    KEEP(CFG_EFFICIENCY);
    KEEP(CFG_MOG_OFF_COUNTDOWN);
    KEEP(CFG_MOG_ON_COUNTDOWN);
    KEEP(CFG_MOG_STAYOFF_COUNTDOWN);
    KEEP_IF(CFG_SOG0_OFF_COUNTDOWN,          has_sog0);
    KEEP_IF(CFG_SOG0_ON_COUNTDOWN,           has_sog0);
    KEEP_IF(CFG_SOG0_STAYOFF_COUNTDOWN,      has_sog0);
    KEEP_IF(CFG_SOG1_OFF_COUNTDOWN,          has_sog1);
    KEEP_IF(CFG_SOG1_ON_COUNTDOWN,           has_sog1);
    KEEP_IF(CFG_SOG1_STAYOFF_COUNTDOWN,      has_sog1);
    KEEP_IF(CFG_SOG2_OFF_COUNTDOWN,          has_sog2);
    KEEP_IF(CFG_SOG2_ON_COUNTDOWN,           has_sog2);
    KEEP_IF(CFG_SOG2_STAYOFF_COUNTDOWN,      has_sog2);

    /* Inventory block — always present */
    KEEP(CFG_FIRMWARE_VERSION);
    KEEP(CFG_MODEL_NAME);
    KEEP(CFG_SKU);
    KEEP(CFG_SERIAL_NUMBER);
    KEEP(CFG_BATTERY_SKU);
    KEEP(CFG_NOMINAL_APPARENT_POWER);
    KEEP(CFG_NOMINAL_REAL_POWER);
    KEEP(CFG_SOG_RELAY_CONFIG);

    /* Names block — UPS and MOG always present, SOG names gated */
    KEEP(CFG_UPS_NAME);
    KEEP(CFG_MOG_NAME);
    KEEP_IF(CFG_SOG0_NAME,                   has_sog0);
    KEEP_IF(CFG_SOG1_NAME,                   has_sog1);
    KEEP_IF(CFG_SOG2_NAME,                   has_sog2);

    #undef KEEP
    #undef KEEP_IF

    return n;
}

/* --- Command descriptors --- */

static const ups_cmd_desc_t smt_commands[] = {
    { "shutdown", "Shutdown UPS", "Send the UPS shutdown command",
      "power", "Shutdown UPS?",
      "This sends the shutdown command directly to the UPS. Use the shutdown workflow for orchestrated multi-host shutdown.",
      UPS_CMD_SIMPLE, UPS_CMD_DANGER, UPS_CMD_IS_SHUTDOWN, 0,
      smt_cmd_shutdown, NULL },

    { "battery_test", "Battery Test", "Run a quick self-test to verify battery health",
      "diagnostics", "Start Battery Test?",
      "This will run a brief self-test where the UPS switches to battery power momentarily to verify battery health.",
      UPS_CMD_SIMPLE, UPS_CMD_DEFAULT, 0, 0,
      smt_cmd_battery_test, NULL },

    { "runtime_cal", "Runtime Calibration", "Deep discharge to recalibrate the runtime estimate",
      "diagnostics", "Start Runtime Calibration?",
      "Runtime calibration deeply discharges the battery to recalibrate the runtime estimate. The UPS will be on battery power for the entire duration. The battery will take a significant amount of time to recharge after calibration completes. The UPS firmware will refuse the test if the load is too low or AC input is unacceptable.",
      UPS_CMD_SIMPLE, UPS_CMD_WARN, 0, 0,
      smt_cmd_runtime_cal, NULL },

    { "abort_runtime_cal", "Abort Calibration", "Cancel a running runtime calibration",
      "diagnostics", "Abort Runtime Calibration?",
      "This will abort the current runtime calibration and return the UPS to normal operation.",
      UPS_CMD_SIMPLE, UPS_CMD_DEFAULT, 0, 0,
      smt_cmd_abort_runtime_cal, NULL },

    { "beep_short", "Short Beep", "Verify the audible alarm is functional",
      "diagnostics", "Short Beep Test?",
      "This will emit a brief beep to verify the audible alarm.",
      UPS_CMD_SIMPLE, UPS_CMD_DEFAULT, 0, 0,
      smt_cmd_beep_short, NULL },

    { "beep_continuous", "Continuous Beep", "Continuous beep and LED test",
      "diagnostics", "Continuous Beep Test?",
      "This starts a continuous beep and LED test. The alarm will sound until you stop it (issue Short Beep again or mute locally).",
      UPS_CMD_SIMPLE, UPS_CMD_WARN, 0, 0,
      smt_cmd_beep_continuous, NULL },

    { "mute", "Mute Alarm", "Silence the UPS audible alarm",
      "alarm", "Mute Alarm?",
      "This will silence the UPS audible alarm. The alarm will remain muted until a new alarm condition occurs or you unmute it.",
      UPS_CMD_SIMPLE, UPS_CMD_DEFAULT, UPS_CMD_IS_MUTE, 0,
      smt_cmd_mute_alarm, NULL },

    { "unmute", "Unmute Alarm", "Re-enable the UPS audible alarm",
      "alarm", "Unmute Alarm?",
      "This will re-enable the UPS audible alarm. Any active alarm conditions will immediately sound.",
      UPS_CMD_SIMPLE, UPS_CMD_DEFAULT, 0, 0,
      smt_cmd_cancel_mute, NULL },

    { "clear_faults", "Clear Faults", "Reset latched fault flags",
      "alarm", "Clear Fault Flags?",
      "This will reset all latched fault indicators on the UPS. Faults may immediately reoccur if their underlying conditions still exist.",
      UPS_CMD_SIMPLE, UPS_CMD_DEFAULT, 0, 0,
      smt_cmd_clear_faults, NULL },
};

/* --- Driver definition --- */

const ups_driver_t ups_driver_smt = {
    .name                = "smt",
    .conn_type           = UPS_CONN_SERIAL,
    .topology            = UPS_TOPO_LINE_INTERACTIVE,
    .connect             = smt_connect,
    .disconnect          = smt_disconnect,
    .detect              = smt_detect,
    .caps                = UPS_CAP_SHUTDOWN | UPS_CAP_BATTERY_TEST | UPS_CAP_RUNTIME_CAL |
                           UPS_CAP_CLEAR_FAULTS | UPS_CAP_MUTE | UPS_CAP_BEEP |
                           UPS_CAP_HE_MODE,
    .freq_settings       = NULL,
    .freq_settings_count = 0,
    .config_regs         = smt_config_regs,
    .config_regs_count   = sizeof(smt_config_regs) / sizeof(smt_config_regs[0]),
    .resolve_config_regs = smt_resolve_config_regs,
    .read_status         = smt_read_status,
    .read_dynamic        = smt_read_dynamic,
    .read_inventory      = smt_read_inventory,
    .read_thresholds     = smt_read_thresholds,
    .read_transfer_reason = smt_read_transfer_reason,
    .commands            = smt_commands,
    .commands_count      = sizeof(smt_commands) / sizeof(smt_commands[0]),
    .config_read         = smt_config_read,
    .config_write        = smt_config_write,
};
