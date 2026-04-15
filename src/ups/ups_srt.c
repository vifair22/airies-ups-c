#include "ups_driver.h"
#include "ups.h"
#include <string.h>

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

/* Frequency tolerance */
#define SRT_REG_FREQ_TOLERANCE 593   /* uint16, FC06 */

/* --- Reads --- */

static int srt_read_status(modbus_t *ctx, ups_data_t *data)
{
    uint16_t regs[SRT_REG_STATUS_LEN];
    if (modbus_read_registers(ctx, SRT_REG_STATUS, SRT_REG_STATUS_LEN, regs) != SRT_REG_STATUS_LEN)
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

static int srt_read_dynamic(modbus_t *ctx, ups_data_t *data)
{
    uint16_t regs[SRT_REG_DYNAMIC_LEN];
    if (modbus_read_registers(ctx, SRT_REG_DYNAMIC, SRT_REG_DYNAMIC_LEN, regs) != SRT_REG_DYNAMIC_LEN)
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

static int srt_read_inventory(modbus_t *ctx, ups_inventory_t *inv)
{
    uint16_t regs[SRT_REG_INVENTORY_LEN];
    if (modbus_read_registers(ctx, SRT_REG_INVENTORY, SRT_REG_INVENTORY_LEN, regs) != SRT_REG_INVENTORY_LEN)
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

static int srt_read_thresholds(modbus_t *ctx, uint16_t *transfer_high, uint16_t *transfer_low)
{
    uint16_t regs[SRT_REG_TRANSFER_LEN];
    if (modbus_read_registers(ctx, SRT_REG_TRANSFER_HIGH, SRT_REG_TRANSFER_LEN, regs) != SRT_REG_TRANSFER_LEN)
        return -1;
    *transfer_high = regs[0];
    *transfer_low = regs[1];
    return 0;
}

/* --- Commands --- */

static int srt_cmd_shutdown(modbus_t *ctx)
{
    return modbus_write_register(ctx, SRT_REG_CMD_SHUTDOWN, 0x0001) == 1 ? 0 : -1;
}

static int srt_cmd_battery_test(modbus_t *ctx)
{
    return modbus_write_register(ctx, SRT_REG_CMD_BATTEST, 0x0001) == 1 ? 0 : -1;
}

static int srt_cmd_runtime_cal(modbus_t *ctx)
{
    return modbus_write_register(ctx, SRT_REG_CMD_RTCAL, 0x0001) == 1 ? 0 : -1;
}

static int srt_cmd_abort_runtime_cal(modbus_t *ctx)
{
    return modbus_write_register(ctx, SRT_REG_CMD_RTCAL, 0x0002) == 1 ? 0 : -1;
}

static int srt_cmd_clear_faults(modbus_t *ctx)
{
    uint16_t cmd[2] = { 0x0000, 0x0200 };
    return modbus_write_registers(ctx, SRT_REG_CMD_UPS, 2, cmd) == 2 ? 0 : -1;
}

static int srt_cmd_mute_alarm(modbus_t *ctx)
{
    return modbus_write_register(ctx, SRT_REG_CMD_UI, 0x0004) == 1 ? 0 : -1;
}

static int srt_cmd_cancel_mute(modbus_t *ctx)
{
    return modbus_write_register(ctx, SRT_REG_CMD_UI, 0x0008) == 1 ? 0 : -1;
}

static int srt_cmd_beep_test(modbus_t *ctx)
{
    return modbus_write_register(ctx, SRT_REG_CMD_UI, 0x0001) == 1 ? 0 : -1;
}

static int srt_cmd_bypass_enable(modbus_t *ctx)
{
    uint16_t cmd[2] = { 0x0000, 0x0010 };
    return modbus_write_registers(ctx, SRT_REG_CMD_UPS, 2, cmd) == 2 ? 0 : -1;
}

static int srt_cmd_bypass_disable(modbus_t *ctx)
{
    uint16_t cmd[2] = { 0x0000, 0x0020 };
    return modbus_write_registers(ctx, SRT_REG_CMD_UPS, 2, cmd) == 2 ? 0 : -1;
}

static int srt_cmd_set_freq_tolerance(modbus_t *ctx, uint16_t setting)
{
    return modbus_write_register(ctx, SRT_REG_FREQ_TOLERANCE, setting) == 1 ? 0 : -1;
}

/* --- Detection --- */

static int srt_detect(const char *model)
{
    return strstr(model, "SRT") != NULL;
}

/* --- Frequency tolerance settings --- */

static const ups_freq_setting_t srt_freq_settings[] = {
    { 1,  "auto",     "Automatic 50/60Hz (47-53, 57-63)", 0 },
    { 2,  "hz50_0_1", "50 Hz +/- 0.1 Hz",                1 },
    { 8,  "hz50_3_0", "50 Hz +/- 3.0 Hz",                0 },
    { 16, "hz60_0_1", "60 Hz +/- 0.1 Hz",                1 },
    { 64, "hz60_3_0", "60 Hz +/- 3.0 Hz",                0 },
};

/* --- Config register descriptors --- */

static const ups_bitfield_opt_t srt_bat_test_opts[] = {
    { 1,  "never",           "Never" },
    { 2,  "onstart_only",    "On startup only" },
    { 4,  "onstart_plus_7",  "On startup + every 7 days" },
    { 8,  "onstart_plus_14", "On startup + every 14 days" },
    { 16, "every_7_since",   "Every 7 days since last test" },
    { 32, "every_14_since",  "Every 14 days since last test" },
};

static const ups_config_reg_t srt_config_regs[] = {
    /* Transfer voltages */
    { "transfer_high", "Input Transfer High Voltage", "V", "transfer",
      1026, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 100, 140 } },
    { "transfer_low", "Input Transfer Low Voltage", "V", "transfer",
      1027, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 90, 110 } },

    /* Battery test interval */
    { "bat_test_interval", "Battery Test Interval", NULL, "battery",
      1024, 1, UPS_CFG_BITFIELD, 1, 1,
      .meta.bitfield = { srt_bat_test_opts,
                         sizeof(srt_bat_test_opts) / sizeof(srt_bat_test_opts[0]) } },

    /* MOG delays */
    { "mog_shutdown_delay", "MOG Shutdown Delay", "s", "delays",
      1029, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 600 } },
    { "mog_start_delay", "MOG Start Delay", "s", "delays",
      1030, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 600 } },

    /* SOG0 delays */
    { "sog0_shutdown_delay", "SOG0 Shutdown Delay", "s", "delays",
      1034, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 600 } },
    { "sog0_start_delay", "SOG0 Start Delay", "s", "delays",
      1035, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 600 } },

    /* SOG1 delays */
    { "sog1_shutdown_delay", "SOG1 Shutdown Delay", "s", "delays",
      1039, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 600 } },
    { "sog1_start_delay", "SOG1 Start Delay", "s", "delays",
      1040, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 600 } },

    /* Output voltage setting (read-only) */
    { "output_voltage_setting", "Output Voltage Setting", NULL, "output",
      592, 1, UPS_CFG_BITFIELD, 1, 0, .meta.bitfield = { NULL, 0 } },

    /* Battery install date */
    { "battery_date", "Battery Install Date", "days since 2000-01-01", "battery",
      595, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 65535 } },
};

/* --- Driver definition --- */

const ups_driver_t ups_driver_srt = {
    .name               = "srt",
    .detect             = srt_detect,
    .caps               = UPS_CAP_SHUTDOWN | UPS_CAP_BATTERY_TEST | UPS_CAP_RUNTIME_CAL |
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
    .cmd_shutdown        = srt_cmd_shutdown,
    .cmd_battery_test    = srt_cmd_battery_test,
    .cmd_runtime_cal     = srt_cmd_runtime_cal,
    .cmd_abort_runtime_cal = srt_cmd_abort_runtime_cal,
    .cmd_clear_faults    = srt_cmd_clear_faults,
    .cmd_mute_alarm      = srt_cmd_mute_alarm,
    .cmd_cancel_mute     = srt_cmd_cancel_mute,
    .cmd_beep_test       = srt_cmd_beep_test,
    .cmd_bypass_enable   = srt_cmd_bypass_enable,
    .cmd_bypass_disable  = srt_cmd_bypass_disable,
    .cmd_set_freq_tolerance = srt_cmd_set_freq_tolerance,
};
