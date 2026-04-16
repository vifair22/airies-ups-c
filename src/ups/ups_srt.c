#include "ups_driver.h"
#include "ups.h"
#include <modbus/modbus.h>
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

/* --- Transport helpers --- */

static modbus_t *mb(void *transport) { return (modbus_t *)transport; }

/* --- Connection lifecycle --- */

static void *srt_connect(const ups_conn_params_t *params)
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

static void srt_disconnect(void *transport)
{
    if (transport) {
        modbus_close(mb(transport));
        modbus_free(mb(transport));
    }
}

static int srt_detect(void *transport)
{
    /* Read model string from inventory block (reg 532, 16 regs = 32 chars) */
    uint16_t regs[16];
    if (modbus_read_registers(mb(transport), 532, 16, regs) != 16)
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

static int srt_read_status(void *transport, ups_data_t *data)
{
    uint16_t regs[SRT_REG_STATUS_LEN];
    if (modbus_read_registers(mb(transport), SRT_REG_STATUS, SRT_REG_STATUS_LEN, regs) != SRT_REG_STATUS_LEN)
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
    if (modbus_read_registers(mb(transport), SRT_REG_DYNAMIC, SRT_REG_DYNAMIC_LEN, regs) != SRT_REG_DYNAMIC_LEN)
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

static int srt_read_inventory(void *transport, ups_inventory_t *inv)
{
    uint16_t regs[SRT_REG_INVENTORY_LEN];
    if (modbus_read_registers(mb(transport), SRT_REG_INVENTORY, SRT_REG_INVENTORY_LEN, regs) != SRT_REG_INVENTORY_LEN)
        return -1;

    memset(inv, 0, sizeof(*inv));

    for (int i = 0; i < 8; i++) {
        inv->firmware[i * 2]     = (char)((regs[i] >> 8) & 0xFF);
        inv->firmware[i * 2 + 1] = (char)(regs[i] & 0xFF);
    }
    for (int i = 0; i < 16; i++) {
        inv->model[i * 2]     = (char)((regs[16 + i] >> 8) & 0xFF);
        inv->model[i * 2 + 1] = (char)(regs[16 + i] & 0xFF);
    }
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

static int srt_read_thresholds(void *transport, uint16_t *transfer_high, uint16_t *transfer_low)
{
    uint16_t regs[SRT_REG_TRANSFER_LEN];
    if (modbus_read_registers(mb(transport), SRT_REG_TRANSFER_HIGH, SRT_REG_TRANSFER_LEN, regs) != SRT_REG_TRANSFER_LEN)
        return -1;
    *transfer_high = regs[0];
    *transfer_low = regs[1];
    return 0;
}

/* --- Config register I/O --- */

static int srt_config_read(void *transport, const ups_config_reg_t *reg,
                           uint16_t *raw_value, char *str_buf, size_t str_bufsz)
{
    uint16_t regs[32];
    int n = reg->reg_count > 0 ? reg->reg_count : 1;
    if (n > 32) n = 32;

    if (modbus_read_registers(mb(transport), reg->reg_addr, n, regs) != n)
        return UPS_ERR_IO;

    if (reg->type == UPS_CFG_STRING && str_buf) {
        size_t max = str_bufsz - 1;
        if (max > (size_t)n * 2) max = (size_t)n * 2;
        for (size_t i = 0; i < max / 2 && i < (size_t)n; i++) {
            str_buf[i * 2]     = (char)((regs[i] >> 8) & 0xFF);
            str_buf[i * 2 + 1] = (char)(regs[i] & 0xFF);
        }
        str_buf[max] = '\0';
    }

    if (raw_value)
        *raw_value = regs[0];

    return UPS_OK;
}

static int srt_config_write(void *transport, const ups_config_reg_t *reg, uint16_t value)
{
    if (modbus_write_register(mb(transport), reg->reg_addr, value) != 1)
        return UPS_ERR_IO;

    /* Inter-write delay (UPS firmware requirement — 100ms minimum) */
    struct timespec delay = { .tv_sec = 0, .tv_nsec = 100000000 };
    nanosleep(&delay, NULL);

    /* Read back for verification */
    uint16_t readback;
    if (modbus_read_registers(mb(transport), reg->reg_addr, 1, &readback) != 1)
        return UPS_ERR_IO;

    if (readback != value)
        return UPS_ERR_IO;

    return UPS_OK;
}

/* --- Commands --- */

static int srt_cmd_shutdown(void *transport)
{
    return modbus_write_register(mb(transport), SRT_REG_CMD_SHUTDOWN, 0x0001) == 1 ? 0 : -1;
}

static int srt_cmd_battery_test(void *transport)
{
    return modbus_write_register(mb(transport), SRT_REG_CMD_BATTEST, 0x0001) == 1 ? 0 : -1;
}

static int srt_cmd_runtime_cal(void *transport)
{
    return modbus_write_register(mb(transport), SRT_REG_CMD_RTCAL, 0x0001) == 1 ? 0 : -1;
}

static int srt_cmd_abort_runtime_cal(void *transport)
{
    return modbus_write_register(mb(transport), SRT_REG_CMD_RTCAL, 0x0002) == 1 ? 0 : -1;
}

static int srt_cmd_clear_faults(void *transport)
{
    uint16_t cmd[2] = { 0x0000, 0x0200 };
    return modbus_write_registers(mb(transport), SRT_REG_CMD_UPS, 2, cmd) == 2 ? 0 : -1;
}

static int srt_cmd_mute_alarm(void *transport)
{
    return modbus_write_register(mb(transport), SRT_REG_CMD_UI, 0x0004) == 1 ? 0 : -1;
}

static int srt_cmd_cancel_mute(void *transport)
{
    return modbus_write_register(mb(transport), SRT_REG_CMD_UI, 0x0008) == 1 ? 0 : -1;
}

static int srt_cmd_beep_short(void *transport)
{
    return modbus_write_register(mb(transport), SRT_REG_CMD_UI, 0x0001) == 1 ? 0 : -1;
}

static int srt_cmd_beep_continuous(void *transport)
{
    return modbus_write_register(mb(transport), SRT_REG_CMD_UI, 0x0002) == 1 ? 0 : -1;
}

static int srt_cmd_bypass_enable(void *transport)
{
    uint16_t cmd[2] = { 0x0000, 0x0010 };
    return modbus_write_registers(mb(transport), SRT_REG_CMD_UPS, 2, cmd) == 2 ? 0 : -1;
}

static int srt_cmd_bypass_disable(void *transport)
{
    uint16_t cmd[2] = { 0x0000, 0x0020 };
    return modbus_write_registers(mb(transport), SRT_REG_CMD_UPS, 2, cmd) == 2 ? 0 : -1;
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
    { "transfer_high", "Upper Acceptable Input Voltage", "V", "transfer",
      1026, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 110, 150 } },
    { "transfer_low", "Lower Acceptable Input Voltage", "V", "transfer",
      1027, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 80, 120 } },
    { "bat_test_interval", "Battery Test Interval", NULL, "battery",
      1024, 1, UPS_CFG_BITFIELD, 1, 1,
      .meta.bitfield = { srt_bat_test_opts,
                         sizeof(srt_bat_test_opts) / sizeof(srt_bat_test_opts[0]) } },
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
                         sizeof(srt_voltage_opts) / sizeof(srt_voltage_opts[0]) } },
    { "freq_tolerance", "Output Frequency Tolerance", NULL, "output",
      593, 1, UPS_CFG_BITFIELD, 1, 1,
      .meta.bitfield = { srt_freq_tol_opts,
                         sizeof(srt_freq_tol_opts) / sizeof(srt_freq_tol_opts[0]) } },
    { "manufacture_date", "Manufacture Date", "days since 2000-01-01", "info",
      591, 1, UPS_CFG_SCALAR, 1, 0, .meta.scalar = { 0, 65535 } },
    { "battery_date", "Battery Install Date", "days since 2000-01-01", "info",
      595, 1, UPS_CFG_SCALAR, 1, 1, .meta.scalar = { 0, 65535 } },
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
    .commands            = srt_commands,
    .commands_count      = sizeof(srt_commands) / sizeof(srt_commands[0]),
    .config_read         = srt_config_read,
    .config_write        = srt_config_write,
};
