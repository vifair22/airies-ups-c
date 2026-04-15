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
    data->bypass_status    = regs[19];
    data->bypass_voltage   = regs[20] / 64.0;
    data->bypass_frequency = regs[21] / 128.0;
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
    { 1,  "auto",     "Automatic 50/60Hz (47-53, 57-63)" },
    { 2,  "hz50_0_1", "50 Hz +/- 0.1 Hz" },
    { 8,  "hz50_3_0", "50 Hz +/- 3.0 Hz" },
    { 16, "hz60_0_1", "60 Hz +/- 0.1 Hz" },
    { 64, "hz60_3_0", "60 Hz +/- 3.0 Hz" },
};

/* --- Config register descriptors --- */

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

static const ups_config_reg_t srt_config_regs[] = {
    /* Transfer voltages — thresholds for switching to/from battery */
    { "transfer_high", "Upper Acceptable Input Voltage", "V", "transfer",
      1026, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 110, 150 } },
    { "transfer_low", "Lower Acceptable Input Voltage", "V", "transfer",
      1027, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 80, 120 } },

    /* Battery test interval */
    { "bat_test_interval", "Battery Test Interval", NULL, "battery",
      1024, 1, UPS_CFG_BITFIELD, 1, 1,
      .meta.bitfield = { srt_bat_test_opts,
                         sizeof(srt_bat_test_opts) / sizeof(srt_bat_test_opts[0]) } },

    /* MOG (Main Outlet Group) delays */
    { "mog_turn_off_delay", "MOG Turn Off Countdown", "s", "outlet_delays",
      1029, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    { "mog_turn_on_delay", "MOG Turn On Countdown", "s", "outlet_delays",
      1030, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },

    /* SOG0 (Switched Outlet Group 0) delays */
    { "sog0_turn_off_delay", "SOG0 Turn Off Countdown", "s", "outlet_delays",
      1034, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    { "sog0_turn_on_delay", "SOG0 Turn On Countdown", "s", "outlet_delays",
      1035, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },

    /* SOG1 (Switched Outlet Group 1) delays */
    { "sog1_turn_off_delay", "SOG1 Turn Off Countdown", "s", "outlet_delays",
      1039, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    { "sog1_turn_on_delay", "SOG1 Turn On Countdown", "s", "outlet_delays",
      1040, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },

    /* Minimum return runtime settings */
    { "mog_min_return_runtime", "MOG Minimum Return Runtime", "s", "outlet_delays",
      1033, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    { "sog0_min_return_runtime", "SOG0 Minimum Return Runtime", "s", "outlet_delays",
      1038, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    { "sog1_min_return_runtime", "SOG1 Minimum Return Runtime", "s", "outlet_delays",
      1043, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },

    /* Load shed config bitfields (2 regs each, we read low word only) */
    { "mog_loadshed_config", "MOG Load Shed Config", NULL, "load_shed",
      1054, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 65535 } },
    { "sog0_loadshed_config", "SOG0 Load Shed Config", NULL, "load_shed",
      1056, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 65535 } },
    { "sog1_loadshed_config", "SOG1 Load Shed Config", NULL, "load_shed",
      1058, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 65535 } },

    /* Load shed: runtime remaining thresholds (seconds, 32767 = disabled) */
    { "mog_loadshed_runtime", "MOG Load Shed Runtime Threshold", "s", "load_shed",
      1072, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    { "sog0_loadshed_runtime", "SOG0 Load Shed Runtime Threshold", "s", "load_shed",
      1064, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    { "sog1_loadshed_runtime", "SOG1 Load Shed Runtime Threshold", "s", "load_shed",
      1065, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },

    /* Load shed: time on battery thresholds (seconds, 32767 = disabled) */
    { "mog_loadshed_time_on_bat", "MOG Load Shed Time on Battery", "s", "load_shed",
      1073, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    { "sog0_loadshed_time_on_bat", "SOG0 Load Shed Time on Battery", "s", "load_shed",
      1068, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },
    { "sog1_loadshed_time_on_bat", "SOG1 Load Shed Time on Battery", "s", "load_shed",
      1069, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 32767 } },

    /* Output voltage setting (read-only) */
    { "output_voltage_setting", "Output Voltage Setting", NULL, "output",
      592, 1, UPS_CFG_BITFIELD, 1, 0,
      .meta.bitfield = { srt_voltage_opts,
                         sizeof(srt_voltage_opts) / sizeof(srt_voltage_opts[0]) } },

    /* Frequency tolerance (read/write) */
    { "freq_tolerance", "Output Frequency Tolerance", NULL, "output",
      593, 1, UPS_CFG_BITFIELD, 1, 1,
      .meta.bitfield = { srt_freq_tol_opts,
                         sizeof(srt_freq_tol_opts) / sizeof(srt_freq_tol_opts[0]) } },

    /* Dates */
    { "manufacture_date", "Manufacture Date", "days since 2000-01-01", "info",
      591, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 65535 } },
    { "battery_date", "Battery Install Date", "days since 2000-01-01", "info",
      595, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 65535 } },

    /* Name strings — available via inventory read, omitted from config
     * register dump to avoid excessive Modbus transactions over serial.
     * Registers: 596 (UPS name), 604 (MOG), 612 (SOG0), 620 (SOG1) */
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
