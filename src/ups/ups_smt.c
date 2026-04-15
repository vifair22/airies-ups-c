#include "ups_driver.h"
#include "ups.h"
#include <string.h>

/* --- SMT register map (990-9840, baseline for SMT750RM2U) ---
 *
 * The SMT series shares the same register map document (990-9840) as the SRT.
 * Register addresses and scaling factors are identical. Differences are
 * capability-based: the SMT is line-interactive (no HE mode, no bypass,
 * no frequency tolerance control).
 *
 * TODO: Verify all reads/commands against actual SMT750RM2U hardware.
 * See APC_SMT_MODBUS_REFERENCE.md for testing notes.
 */

/* Status block: reg 0, 27 registers */
#define SMT_REG_STATUS       0
#define SMT_REG_STATUS_LEN   27

/* Dynamic block: reg 128, 44 registers */
#define SMT_REG_DYNAMIC      128
#define SMT_REG_DYNAMIC_LEN  44

/* Inventory block: reg 516, 80 registers */
#define SMT_REG_INVENTORY      516
#define SMT_REG_INVENTORY_LEN  80

/* Transfer thresholds */
#define SMT_REG_TRANSFER_HIGH  1026
#define SMT_REG_TRANSFER_LEN   2

/* Command registers */
#define SMT_REG_CMD_UPS        1536  /* uint32, FC16: clear faults */
#define SMT_REG_CMD_SHUTDOWN   1540  /* uint16, FC06: simple signaling shutdown */
#define SMT_REG_CMD_BATTEST    1541  /* uint16, FC06: battery test */
#define SMT_REG_CMD_RTCAL      1542  /* uint16, FC06: runtime calibration */
#define SMT_REG_CMD_UI         1543  /* uint16, FC06: beep, mute */

/* --- Reads ---
 * Register layout matches SRT per 990-9840. Parsing is identical. */

static int smt_read_status(modbus_t *ctx, ups_data_t *data)
{
    uint16_t regs[SMT_REG_STATUS_LEN];
    if (modbus_read_registers(ctx, SMT_REG_STATUS, SMT_REG_STATUS_LEN, regs) != SMT_REG_STATUS_LEN)
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

static int smt_read_dynamic(modbus_t *ctx, ups_data_t *data)
{
    uint16_t regs[SMT_REG_DYNAMIC_LEN];
    if (modbus_read_registers(ctx, SMT_REG_DYNAMIC, SMT_REG_DYNAMIC_LEN, regs) != SMT_REG_DYNAMIC_LEN)
        return -1;

    data->runtime_sec      = ((uint32_t)regs[0] << 16) | regs[1];
    data->charge_pct       = regs[2] / 512.0;
    data->battery_voltage  = (int16_t)regs[3] / 32.0;
    data->load_pct         = regs[8] / 256.0;
    data->output_current   = regs[12] / 32.0;
    data->output_voltage   = regs[14] / 64.0;
    data->output_frequency = regs[16] / 128.0;
    data->output_energy_wh = ((uint32_t)regs[17] << 16) | regs[18];
    data->input_status     = regs[22];
    data->input_voltage    = regs[23] / 64.0;
    data->efficiency       = (int16_t)regs[26] / 128.0;
    data->timer_shutdown   = (int16_t)regs[27];
    data->timer_start      = (int16_t)regs[28];
    data->timer_reboot     = ((int32_t)regs[29] << 16) | regs[30];

    return 0;
}

static int smt_read_inventory(modbus_t *ctx, ups_inventory_t *inv)
{
    uint16_t regs[SMT_REG_INVENTORY_LEN];
    if (modbus_read_registers(ctx, SMT_REG_INVENTORY, SMT_REG_INVENTORY_LEN, regs) != SMT_REG_INVENTORY_LEN)
        return -1;

    memset(inv, 0, sizeof(*inv));

    /* Firmware: reg 516, 8 regs = 16 chars */
    for (int i = 0; i < 8; i++) {
        inv->firmware[i * 2]     = (char)((regs[i] >> 8) & 0xFF);
        inv->firmware[i * 2 + 1] = (char)(regs[i] & 0xFF);
    }

    /* Model: reg 532, 16 regs = 32 chars */
    for (int i = 0; i < 16; i++) {
        inv->model[i * 2]     = (char)((regs[16 + i] >> 8) & 0xFF);
        inv->model[i * 2 + 1] = (char)(regs[16 + i] & 0xFF);
    }

    /* Serial: reg 564, 8 regs = 16 chars */
    for (int i = 0; i < 8; i++) {
        inv->serial[i * 2]     = (char)((regs[48 + i] >> 8) & 0xFF);
        inv->serial[i * 2 + 1] = (char)(regs[48 + i] & 0xFF);
    }

    inv->nominal_va     = regs[72];
    inv->nominal_watts  = regs[73];
    inv->sog_config     = regs[74];
    inv->freq_tolerance = regs[77];

    return 0;
}

static int smt_read_thresholds(modbus_t *ctx, uint16_t *transfer_high, uint16_t *transfer_low)
{
    uint16_t regs[SMT_REG_TRANSFER_LEN];
    if (modbus_read_registers(ctx, SMT_REG_TRANSFER_HIGH, SMT_REG_TRANSFER_LEN, regs) != SMT_REG_TRANSFER_LEN)
        return -1;
    *transfer_high = regs[0];
    *transfer_low = regs[1];
    return 0;
}

/* --- Commands --- */

static int smt_cmd_shutdown(modbus_t *ctx)
{
    return modbus_write_register(ctx, SMT_REG_CMD_SHUTDOWN, 0x0001) == 1 ? 0 : -1;
}

static int smt_cmd_battery_test(modbus_t *ctx)
{
    return modbus_write_register(ctx, SMT_REG_CMD_BATTEST, 0x0001) == 1 ? 0 : -1;
}

static int smt_cmd_runtime_cal(modbus_t *ctx)
{
    return modbus_write_register(ctx, SMT_REG_CMD_RTCAL, 0x0001) == 1 ? 0 : -1;
}

static int smt_cmd_abort_runtime_cal(modbus_t *ctx)
{
    return modbus_write_register(ctx, SMT_REG_CMD_RTCAL, 0x0002) == 1 ? 0 : -1;
}

static int smt_cmd_clear_faults(modbus_t *ctx)
{
    uint16_t cmd[2] = { 0x0000, 0x0200 };
    return modbus_write_registers(ctx, SMT_REG_CMD_UPS, 2, cmd) == 2 ? 0 : -1;
}

static int smt_cmd_mute_alarm(modbus_t *ctx)
{
    return modbus_write_register(ctx, SMT_REG_CMD_UI, 0x0004) == 1 ? 0 : -1;
}

static int smt_cmd_cancel_mute(modbus_t *ctx)
{
    return modbus_write_register(ctx, SMT_REG_CMD_UI, 0x0008) == 1 ? 0 : -1;
}

static int smt_cmd_beep_test(modbus_t *ctx)
{
    return modbus_write_register(ctx, SMT_REG_CMD_UI, 0x0001) == 1 ? 0 : -1;
}

/* --- Detection --- */

static int smt_detect(const char *model)
{
    return strstr(model, "SMT") != NULL;
}

/* --- Driver definition ---
 *
 * Baseline capabilities. The SMT is line-interactive, so no HE mode,
 * no bypass control, and no frequency tolerance manipulation.
 * These may need adjustment after hardware testing. */

const ups_driver_t ups_driver_smt = {
    .name               = "smt",
    .detect             = smt_detect,
    .caps               = UPS_CAP_SHUTDOWN | UPS_CAP_BATTERY_TEST | UPS_CAP_RUNTIME_CAL |
                          UPS_CAP_CLEAR_FAULTS | UPS_CAP_MUTE | UPS_CAP_BEEP,
    .freq_settings       = NULL,
    .freq_settings_count = 0,
    .config_regs         = NULL,  /* TODO: populate after SMT hardware testing */
    .config_regs_count   = 0,
    .read_status         = smt_read_status,
    .read_dynamic        = smt_read_dynamic,
    .read_inventory      = smt_read_inventory,
    .read_thresholds     = smt_read_thresholds,
    .cmd_shutdown        = smt_cmd_shutdown,
    .cmd_battery_test    = smt_cmd_battery_test,
    .cmd_runtime_cal     = smt_cmd_runtime_cal,
    .cmd_abort_runtime_cal = smt_cmd_abort_runtime_cal,
    .cmd_clear_faults    = smt_cmd_clear_faults,
    .cmd_mute_alarm      = smt_cmd_mute_alarm,
    .cmd_cancel_mute     = smt_cmd_cancel_mute,
    .cmd_beep_test       = smt_cmd_beep_test,
    .cmd_bypass_enable   = NULL,
    .cmd_bypass_disable  = NULL,
    .cmd_set_freq_tolerance = NULL,
};
