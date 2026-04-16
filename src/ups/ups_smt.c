#include "ups_driver.h"
#include "ups.h"
#include <modbus/modbus.h>
#include <string.h>

/* --- SMT register map (990-9840, baseline for SMT750RM2U) ---
 *
 * The SMT series shares the same register map document (990-9840) as the SRT.
 * Register addresses and scaling factors are identical. Differences are
 * capability-based: the SMT is line-interactive (no bypass control,
 * no frequency tolerance control). HE mode (bit 13) is supported per
 * the register map but needs hardware verification.
 *
 * TODO: Verify all reads/commands against actual SMT750RM2U hardware.
 */

#define SMT_REG_STATUS       0
#define SMT_REG_STATUS_LEN   27
#define SMT_REG_DYNAMIC      128
#define SMT_REG_DYNAMIC_LEN  44
#define SMT_REG_INVENTORY      516
#define SMT_REG_INVENTORY_LEN  80
#define SMT_REG_TRANSFER_HIGH  1026
#define SMT_REG_TRANSFER_LEN   2
#define SMT_REG_CMD_UPS        1536
#define SMT_REG_CMD_SHUTDOWN   1540
#define SMT_REG_CMD_BATTEST    1541
#define SMT_REG_CMD_RTCAL      1542
#define SMT_REG_CMD_UI         1543

/* --- Transport helper --- */

static modbus_t *mb(void *transport) { return (modbus_t *)transport; }

/* --- Connection lifecycle (shared Modbus RTU pattern) --- */

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

static int smt_detect(void *transport)
{
    uint16_t regs[16];
    if (modbus_read_registers(mb(transport), 532, 16, regs) != 16)
        return 0;

    char model[33];
    memset(model, 0, sizeof(model));
    for (int i = 0; i < 16; i++) {
        model[i * 2]     = (char)((regs[i] >> 8) & 0xFF);
        model[i * 2 + 1] = (char)(regs[i] & 0xFF);
    }

    return strstr(model, "SMT") != NULL;
}

/* --- Reads --- */

static int smt_read_status(void *transport, ups_data_t *data)
{
    uint16_t regs[SMT_REG_STATUS_LEN];
    if (modbus_read_registers(mb(transport), SMT_REG_STATUS, SMT_REG_STATUS_LEN, regs) != SMT_REG_STATUS_LEN)
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
    if (modbus_read_registers(mb(transport), SMT_REG_DYNAMIC, SMT_REG_DYNAMIC_LEN, regs) != SMT_REG_DYNAMIC_LEN)
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

static int smt_read_inventory(void *transport, ups_inventory_t *inv)
{
    uint16_t regs[SMT_REG_INVENTORY_LEN];
    if (modbus_read_registers(mb(transport), SMT_REG_INVENTORY, SMT_REG_INVENTORY_LEN, regs) != SMT_REG_INVENTORY_LEN)
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

static int smt_read_thresholds(void *transport, uint16_t *transfer_high, uint16_t *transfer_low)
{
    uint16_t regs[SMT_REG_TRANSFER_LEN];
    if (modbus_read_registers(mb(transport), SMT_REG_TRANSFER_HIGH, SMT_REG_TRANSFER_LEN, regs) != SMT_REG_TRANSFER_LEN)
        return -1;
    *transfer_high = regs[0];
    *transfer_low = regs[1];
    return 0;
}

/* --- Commands --- */

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

/* --- Driver definition --- */

const ups_driver_t ups_driver_smt = {
    .name                = "smt",
    .conn_type           = UPS_CONN_SERIAL,
    .topology            = UPS_TOPO_LINE_INTERACTIVE,
    .connect             = smt_connect,
    .disconnect          = smt_disconnect,
    .detect              = smt_detect,
    .caps                = UPS_CAP_SHUTDOWN | UPS_CAP_BATTERY_TEST | UPS_CAP_RUNTIME_CAL |
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
    .cmd_beep_short      = smt_cmd_beep_short,
    .cmd_beep_continuous = smt_cmd_beep_continuous,
    .cmd_bypass_enable   = NULL,
    .cmd_bypass_disable  = NULL,
    .cmd_set_freq_tolerance = NULL,
    .config_read         = NULL,
    .config_write        = NULL,
};
