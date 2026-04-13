#include "ups.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>

modbus_t *ups_connect(const char *device, int baud, int slave_id)
{
    modbus_t *ctx = modbus_new_rtu(device, baud, 'N', 8, 1);
    if (!ctx) return NULL;

    modbus_set_slave(ctx, slave_id);
    modbus_set_response_timeout(ctx, 3, 0);

    if (modbus_connect(ctx) == -1) {
        modbus_free(ctx);
        return NULL;
    }

    return ctx;
}

void ups_close(modbus_t *ctx)
{
    if (ctx) {
        modbus_close(ctx);
        modbus_free(ctx);
    }
}

int ups_read_status(modbus_t *ctx, ups_data_t *data)
{
    uint16_t regs[27];
    if (modbus_read_registers(ctx, 0, 27, regs) != 27)
        return -1;

    data->status          = ((uint32_t)regs[0] << 16) | regs[1];
    data->transfer_reason = regs[2];
    data->outlet_mog      = ((uint32_t)regs[3] << 16) | regs[4];
    data->outlet_sog0     = ((uint32_t)regs[6] << 16) | regs[7];
    data->outlet_sog1     = ((uint32_t)regs[9] << 16) | regs[10];
    data->sig_status      = regs[18];
    data->bat_error       = regs[19];
    data->bat_test_status = regs[23];
    data->rt_cal_status   = regs[24];

    return 0;
}

int ups_read_dynamic(modbus_t *ctx, ups_data_t *data)
{
    uint16_t regs[44];
    if (modbus_read_registers(ctx, 128, 44, regs) != 44)
        return -1;

    data->runtime_sec     = ((uint32_t)regs[0] << 16) | regs[1];
    data->charge_pct      = regs[2] / 512.0;
    data->battery_voltage = (int16_t)regs[3] / 32.0;
    data->load_pct        = regs[8] / 256.0;
    data->output_current  = regs[12] / 32.0;
    data->output_voltage  = regs[14] / 64.0;
    data->output_frequency = regs[16] / 128.0;
    data->output_energy_wh = ((uint32_t)regs[17] << 16) | regs[18];
    data->input_voltage   = regs[23] / 64.0;
    data->efficiency      = (int16_t)regs[26] / 128.0;
    data->timer_shutdown  = (int16_t)regs[27];
    data->timer_start     = (int16_t)regs[28];
    data->timer_reboot    = ((int32_t)regs[29] << 16) | regs[30];

    return 0;
}

int ups_read_inventory(modbus_t *ctx, ups_inventory_t *inv)
{
    uint16_t regs[80];
    if (modbus_read_registers(ctx, 516, 80, regs) != 80)
        return -1;

    memset(inv, 0, sizeof(*inv));

    /* Firmware: reg 516, 8 regs = 16 chars */
    for (int i = 0; i < 8; i++) {
        inv->firmware[i * 2]     = (regs[i] >> 8) & 0xFF;
        inv->firmware[i * 2 + 1] = regs[i] & 0xFF;
    }

    /* Model: reg 532, 16 regs = 32 chars */
    for (int i = 0; i < 16; i++) {
        inv->model[i * 2]     = (regs[16 + i] >> 8) & 0xFF;
        inv->model[i * 2 + 1] = regs[16 + i] & 0xFF;
    }

    /* Serial: reg 564, 8 regs = 16 chars */
    for (int i = 0; i < 8; i++) {
        inv->serial[i * 2]     = (regs[48 + i] >> 8) & 0xFF;
        inv->serial[i * 2 + 1] = regs[48 + i] & 0xFF;
    }

    inv->nominal_va     = regs[72];
    inv->nominal_watts  = regs[73];
    inv->sog_config     = regs[74];
    inv->operating_mode = regs[77];

    return 0;
}

/* Commands */

int ups_cmd_shutdown(modbus_t *ctx)
{
    return modbus_write_register(ctx, 1540, 0x0001) == 1 ? 0 : -1;
}

int ups_cmd_clear_faults(modbus_t *ctx)
{
    uint16_t cmd[2] = { 0x0000, 0x0200 };
    return modbus_write_registers(ctx, 1536, 2, cmd) == 2 ? 0 : -1;
}

int ups_cmd_battery_test(modbus_t *ctx)
{
    return modbus_write_register(ctx, 1541, 0x0001) == 1 ? 0 : -1;
}

int ups_cmd_mute_alarm(modbus_t *ctx)
{
    return modbus_write_register(ctx, 1543, 0x0004) == 1 ? 0 : -1;
}

int ups_cmd_cancel_mute(modbus_t *ctx)
{
    return modbus_write_register(ctx, 1543, 0x0008) == 1 ? 0 : -1;
}

int ups_cmd_beep_test(modbus_t *ctx)
{
    return modbus_write_register(ctx, 1543, 0x0001) == 1 ? 0 : -1;
}

/* String helpers */

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
        { UPS_ST_ONLINE,       "Online" },
        { UPS_ST_ON_BATTERY,   "OnBattery" },
        { UPS_ST_BYPASS,       "Bypass" },
        { UPS_ST_OUTPUT_OFF,   "OutputOff" },
        { UPS_ST_FAULT,        "Fault" },
        { UPS_ST_INPUT_BAD,    "InputBad" },
        { UPS_ST_TEST,         "SelfTest" },
        { UPS_ST_SHUT_PENDING, "ShutdownPending" },
        { UPS_ST_HE_MODE,      "HighEfficiency" },
        { UPS_ST_OVERLOAD,     "Overload" },
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
