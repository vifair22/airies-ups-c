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
 * APC_SMT_MODBUS_REFERENCE.md at the repo root.
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
 * line — see APC_SMT_MODBUS_REFERENCE.md "Quirk — driver must detect via
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
                           uint16_t *raw_value, char *str_buf, size_t str_bufsz)
{
    uint16_t regs[32];
    int n = reg->reg_count > 0 ? reg->reg_count : 1;
    if (n > 32) n = 32;

    if (modbus_read_registers(mb(transport), reg->reg_addr, n, regs) != n)
        return UPS_ERR_IO;

    if (reg->type == UPS_CFG_STRING && str_buf) {
        smt_str_decode(regs, n, str_buf, str_bufsz);
    }

    if (raw_value)
        *raw_value = regs[0];

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

    CFG_COUNT
};

static const ups_bitfield_opt_t smt_bat_test_opts[] = {
    { 1,  "never",         "Never" },
    { 2,  "onstart_only",  "Startup Only" },
    { 4,  "onstart_plus_7","Startup + Every 7 Days" },
    { 8,  "onstart_plus_14","Startup + Every 14 Days" },
    { 16, "onstart_7d",    "Startup + 7 Days Since Test" },
    { 32, "onstart_14d",   "Startup + 14 Days Since Test" },
};

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
                           sizeof(smt_bat_test_opts)/sizeof(smt_bat_test_opts[0]) } },
    [CFG_SENSITIVITY] = {
        "sensitivity", "Line Sensitivity", NULL, "input",
        1028, 1, UPS_CFG_BITFIELD, 1, 1,
        .meta.bitfield = { smt_sensitivity_opts,
                           sizeof(smt_sensitivity_opts)/sizeof(smt_sensitivity_opts[0]) } },
    [CFG_OUTPUT_VOLTAGE_SETTING] = {
        "output_voltage_setting", "Output Voltage Setting", NULL, "output",
        592, 1, UPS_CFG_BITFIELD, 1, 0,
        .meta.bitfield = { smt_voltage_opts,
                           sizeof(smt_voltage_opts)/sizeof(smt_voltage_opts[0]) } },
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
    [CFG_MOG_LOADSHED_CONFIG] = {
        "mog_loadshed_config", "MOG Load Shed Config", NULL, "load_shed",
        1054, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 65535 } },
    [CFG_MOG_LOADSHED_RUNTIME] = {
        "mog_loadshed_runtime", "MOG Load Shed Runtime Threshold", "s", "load_shed",
        1072, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    [CFG_MOG_LOADSHED_TIME_ON_BAT] = {
        "mog_loadshed_time_on_bat", "MOG Load Shed Time on Battery", "s", "load_shed",
        1073, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },

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
    [CFG_SOG0_LOADSHED_CONFIG] = {
        "sog0_loadshed_config", "SOG0 Load Shed Config", NULL, "load_shed",
        1056, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 65535 } },
    [CFG_SOG0_LOADSHED_RUNTIME] = {
        "sog0_loadshed_runtime", "SOG0 Load Shed Runtime Threshold", "s", "load_shed",
        1064, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    [CFG_SOG0_LOADSHED_TIME_ON_BAT] = {
        "sog0_loadshed_time_on_bat", "SOG0 Load Shed Time on Battery", "s", "load_shed",
        1068, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },

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
    [CFG_SOG1_LOADSHED_CONFIG] = {
        "sog1_loadshed_config", "SOG1 Load Shed Config", NULL, "load_shed",
        1058, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 65535 } },
    [CFG_SOG1_LOADSHED_RUNTIME] = {
        "sog1_loadshed_runtime", "SOG1 Load Shed Runtime Threshold", "s", "load_shed",
        1065, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    [CFG_SOG1_LOADSHED_TIME_ON_BAT] = {
        "sog1_loadshed_time_on_bat", "SOG1 Load Shed Time on Battery", "s", "load_shed",
        1069, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },

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
    [CFG_SOG2_LOADSHED_CONFIG] = {
        "sog2_loadshed_config", "SOG2 Load Shed Config", NULL, "load_shed",
        1060, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 65535 } },
    [CFG_SOG2_LOADSHED_RUNTIME] = {
        "sog2_loadshed_runtime", "SOG2 Load Shed Runtime Threshold", "s", "load_shed",
        1066, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    [CFG_SOG2_LOADSHED_TIME_ON_BAT] = {
        "sog2_loadshed_time_on_bat", "SOG2 Load Shed Time on Battery", "s", "load_shed",
        1070, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
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
    .commands            = smt_commands,
    .commands_count      = sizeof(smt_commands) / sizeof(smt_commands[0]),
    .config_read         = smt_config_read,
    .config_write        = smt_config_write,
};
