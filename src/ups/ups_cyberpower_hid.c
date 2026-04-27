#include "hid_pdc_core.h"
#include "ups.h"
#include "ups_driver.h"

#include <cutils/log.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- CyberPower HID Power Device driver ---
 *
 * Covers CyberPower PowerPanel HID devices at VID 0x0764. PID 0x0601 is
 * the common PowerPanel Personal series id used by CP1500PFCLCD,
 * CP1500AVRLCD, CP1350AVRLCD, and many siblings. Add new entries to
 * cyberpower_vid_pid[] as they're identified.
 *
 * CyberPower implements the standard HID PDC (pages 0x84 / 0x85) cleanly
 * and does NOT expose useful vendor-page commands. This driver therefore
 * delegates everything to the shared hid_pdc_core and adds only:
 *
 *   - VID/PID precheck and identification by manufacturer string
 *   - A nameplate lookup table for SKUs whose descriptor lacks
 *     ConfigApparentPower / ConfigActivePower
 *
 * NUT covers the same hardware via its generic usbhid-ups subdriver; the
 * standard PDC contract is sufficient for status, telemetry, shutdown,
 * battery test, mute, and the operator-tunable delay settings. */

typedef struct {
    uint16_t vid;
    uint16_t pid;
} cyberpower_vid_pid_t;

static const cyberpower_vid_pid_t cyberpower_vid_pid[] = {
    { 0x0764, 0x0601 },  /* PowerPanel Personal: CP1500PFCLCD and many siblings */
};

/* --- Known nameplate ratings (fallback when descriptor lacks them) ------ */

typedef struct {
    const char *model;
    uint16_t    va;
    uint16_t    watts;
} cyberpower_model_spec_t;

/* Sourced from CyberPower's published spec sheets. Sorted alphabetically.
 * Match is substring-based against the USB product string. */
static const cyberpower_model_spec_t cyberpower_known_models[] = {
    { "CP1000PFCLCD", 1000,  600 },
    { "CP1350AVRLCD", 1350,  815 },
    { "CP1500AVRLCD", 1500,  900 },
    { "CP1500PFCLCD", 1500, 1000 },
    { "CP1500PFCRM2U", 1500, 1000 },
};

/* --- Lifecycle ----------------------------------------------------------- */

static int claims_vid_pid(uint16_t vid, uint16_t pid)
{
    for (size_t i = 0; i < sizeof(cyberpower_vid_pid) / sizeof(cyberpower_vid_pid[0]); i++) {
        if (cyberpower_vid_pid[i].vid == vid && cyberpower_vid_pid[i].pid == pid)
            return 1;
    }
    return 0;
}

static void *cyberpower_connect(const ups_conn_params_t *params)
{
    if (!params || params->type != UPS_CONN_USB) return NULL;
    if (!claims_vid_pid(params->usb.vendor_id, params->usb.product_id)) return NULL;

    hid_pdc_transport_t *t = hid_pdc_open(params);
    if (!t) return NULL;

    /* PowerPanel HID units in this VID/PID range are line-interactive
     * with PFC sinewave output, but their HID descriptor doesn't declare
     * a PowerConverter collection — so the core's auto-detect would
     * default them to STANDBY. Override here. Confirmed against
     * CP1500PFCLCD; matches CyberPower's own product positioning for
     * the entire CP/PR/OR PowerPanel Personal line. */
    t->topology = UPS_TOPO_LINE_INTERACTIVE;
    log_info("cyberpower_hid: topology overridden to line-interactive "
             "(CyberPower omits PowerConverter collection from descriptor)");

    /* Boost/Buck (HID PDC 0x84:0x6E / 0x6F) are unreliable on CyberPower
     * PowerPanel HID — these units don't have an AVR tap-changer (they're
     * line-interactive in the PFC sinewave sense) but firmware exposes
     * the bits anyway, often stuck-on. Skip them here so the dashboard
     * doesn't permanently flag the unit as boosting. NUT's CPS subdriver
     * does the same. */
    t->skip_avr_bits = 1;

    return t;
}

static void cyberpower_disconnect(void *transport)
{
    hid_pdc_transport_t *t = transport;
    if (!t) return;
    /* No vendor-owned state to free. */
    hid_pdc_close(t);
}

static int cyberpower_detect(void *transport)
{
    hid_pdc_transport_t *t = transport;

    if (t->pdc.remaining_capacity) {
        int32_t cap = hid_field_read_raw(t->fd, t->pdc.remaining_capacity);
        if (cap == 0)
            cap = hid_field_read_raw(t->fd, t->pdc.remaining_capacity);
        (void)cap;
    }

    char mfg[128];
    hid_pdc_read_sysfs_string(t->sysfs_base, "manufacturer", mfg, sizeof(mfg));
    if (strstr(mfg, "CPS") != NULL || strstr(mfg, "Cyber Power") != NULL)
        return 1;

    /* Fall back to product-string heuristic — PowerPanel models all start
     * with CP/PR/OR/OL prefixes. */
    char product[128];
    hid_pdc_read_sysfs_string(t->sysfs_base, "product", product, sizeof(product));
    if (strncmp(product, "CP", 2) == 0 ||
        strncmp(product, "PR", 2) == 0 ||
        strncmp(product, "OR", 2) == 0 ||
        strncmp(product, "OL", 2) == 0)
        return 1;

    return 0;
}

static ups_topology_t cyberpower_get_topology(void *transport)
{
    hid_pdc_transport_t *t = transport;
    return t->topology;
}

/* --- Reads --------------------------------------------------------------- */

static int cyberpower_read_status(void *transport, ups_data_t *data)
{
    return hid_pdc_read_status_standard(transport, data);
}

static int cyberpower_read_dynamic(void *transport, ups_data_t *data)
{
    return hid_pdc_read_dynamic_standard(transport, data);
}

static int cyberpower_read_inventory(void *transport, ups_inventory_t *inv)
{
    hid_pdc_transport_t *t = transport;
    memset(inv, 0, sizeof(*inv));

    hid_pdc_read_sysfs_string(t->sysfs_base, "product", inv->model, sizeof(inv->model));
    hid_pdc_read_sysfs_string(t->sysfs_base, "serial", inv->serial, sizeof(inv->serial));

    /* CyberPower exposes firmware via its own descriptor field; the
     * sysfs product string typically does not embed it. Leave inv->firmware
     * empty and rely on the resolve-fields log if the friend's unit
     * surfaces something useful. */

    inv->nominal_va    = 0;
    inv->nominal_watts = 0;

    if (t->pdc.config_apparent_power) {
        int32_t va = hid_field_read_raw(t->fd, t->pdc.config_apparent_power);
        if (va > 0) inv->nominal_va = (uint16_t)va;
    }
    if (t->pdc.config_active_power) {
        int32_t w = hid_field_read_raw(t->fd, t->pdc.config_active_power);
        if (w > 0) inv->nominal_watts = (uint16_t)w;
    }

    if (inv->nominal_va > 0)
        log_info("cyberpower_hid: VA from descriptor — %u VA", inv->nominal_va);
    if (inv->nominal_watts > 0)
        log_info("cyberpower_hid: watts from descriptor — %u W", inv->nominal_watts);

    if (inv->nominal_va == 0 || inv->nominal_watts == 0) {
        for (size_t i = 0; i < sizeof(cyberpower_known_models) / sizeof(cyberpower_known_models[0]); i++) {
            if (strstr(inv->model, cyberpower_known_models[i].model)) {
                if (inv->nominal_va == 0) {
                    inv->nominal_va = cyberpower_known_models[i].va;
                    log_info("cyberpower_hid: VA from model lookup '%s' — %u VA",
                             cyberpower_known_models[i].model, inv->nominal_va);
                }
                if (inv->nominal_watts == 0) {
                    inv->nominal_watts = cyberpower_known_models[i].watts;
                    log_info("cyberpower_hid: watts from model lookup '%s' — %u W",
                             cyberpower_known_models[i].model, inv->nominal_watts);
                }
                break;
            }
        }
    }

    if (inv->nominal_va == 0)
        log_warn("cyberpower_hid: could not determine VA rating");
    if (inv->nominal_watts == 0)
        log_warn("cyberpower_hid: could not determine watt rating");

    inv->sog_config    = 0;
    t->nominal_watts   = inv->nominal_watts;
    return 0;
}

static int cyberpower_read_thresholds(void *transport,
                                       uint16_t *transfer_high,
                                       uint16_t *transfer_low)
{
    return hid_pdc_read_thresholds_standard(transport, transfer_high, transfer_low);
}

/* --- Commands ------------------------------------------------------------ */

static int cp_cmd_shutdown(void *transport)       { return hid_pdc_cmd_shutdown(transport); }
static int cp_cmd_abort_shutdown(void *transport) { return hid_pdc_cmd_abort_shutdown(transport); }
static int cp_cmd_battery_test(void *transport)   { return hid_pdc_cmd_battery_test(transport); }
static int cp_cmd_mute(void *transport)           { return hid_pdc_cmd_mute_alarm(transport); }
static int cp_cmd_unmute(void *transport)         { return hid_pdc_cmd_unmute_alarm(transport); }
static int cp_cmd_clear_faults(void *transport)   { return hid_pdc_cmd_clear_faults(transport); }

/* --- Config registers ---------------------------------------------------- */

static const ups_bitfield_opt_t alarm_opts[] = {
    { 1, "disabled", "Disabled" },
    { 2, "enabled",  "Enabled" },
    { 3, "muted",    "Muted" },
};

/* Standard PDC descriptor set only — no CyberPower vendor extensions in
 * the first cut. Add to this table if a CyberPower vendor-page register
 * proves useful in production. */
static ups_config_reg_t cyberpower_config_regs[] = {
    /* --- Group: transfer --- */
    { "transfer_low", "Low Transfer Voltage", "V", "transfer",
      0, 1, UPS_CFG_SCALAR, 1, 1,
      .meta.scalar = { 0, 0 } },
    { "transfer_high", "High Transfer Voltage", "V", "transfer",
      0, 1, UPS_CFG_SCALAR, 1, 1,
      .meta.scalar = { 0, 0 } },

    /* --- Group: alarm --- */
    { "alarm_setting", "Audible Alarm", NULL, "alarm",
      0, 1, UPS_CFG_BITFIELD, 1, 1,
      .meta.bitfield = { alarm_opts,
                         sizeof(alarm_opts) / sizeof(alarm_opts[0]),
                         1 } },

    /* --- Group: battery --- */
    { "low_battery_warning", "Low Runtime Warning", "s", "battery",
      0, 1, UPS_CFG_SCALAR, 1, 1,
      .meta.scalar = { 0, 0 } },
    { "low_battery_threshold", "Low Battery Shutdown", "%", "battery",
      0, 1, UPS_CFG_SCALAR, 1, 1,
      .meta.scalar = { 0, 0 } },
    { "warning_capacity", "Warning Capacity Level", "%", "battery",
      0, 1, UPS_CFG_SCALAR, 1, 1,
      .meta.scalar = { 0, 0 } },
    { "manufacture_date", "Manufacture Date", "days since 2000-01-01", "info",
      0, 1, UPS_CFG_SCALAR, 1, 0,
      .meta.scalar = { 0, 0 },
      .category = UPS_REG_CATEGORY_IDENTITY },

    /* --- Group: info --- */
    { "nominal_voltage", "Nominal Input Voltage", "V", "info",
      0, 1, UPS_CFG_SCALAR, 1, 0,
      .meta.scalar = { 0, 0 },
      .category = UPS_REG_CATEGORY_IDENTITY },
    { "nominal_va", "Nominal Apparent Power", "VA", "info",
      0, 1, UPS_CFG_SCALAR, 1, 0,
      .meta.scalar = { 0, 0 },
      .category = UPS_REG_CATEGORY_IDENTITY },
    { "design_capacity", "Design Capacity", "%", "info",
      0, 1, UPS_CFG_SCALAR, 1, 0,
      .meta.scalar = { 0, 0 },
      .category = UPS_REG_CATEGORY_IDENTITY },

    /* --- Live measurements --- */
    { "input_voltage", "Input Voltage", "V", "input",
      0, 1, UPS_CFG_SCALAR, 1, 0,
      .meta.scalar = { 0, 0 },
      .category = UPS_REG_CATEGORY_MEASUREMENT },
    { "battery_voltage", "Battery Voltage", "V", "battery",
      0, 1, UPS_CFG_SCALAR, 1, 0,
      .meta.scalar = { 0, 0 },
      .category = UPS_REG_CATEGORY_MEASUREMENT },
    { "remaining_capacity", "Battery Charge", "%", "battery",
      0, 1, UPS_CFG_SCALAR, 1, 0,
      .meta.scalar = { 0, 0 },
      .category = UPS_REG_CATEGORY_MEASUREMENT },
    { "runtime_to_empty", "Runtime Remaining", "s", "battery",
      0, 1, UPS_CFG_SCALAR, 1, 0,
      .meta.scalar = { 0, 0 },
      .category = UPS_REG_CATEGORY_MEASUREMENT },
    { "percent_load", "Output Load", "%", "output",
      0, 1, UPS_CFG_SCALAR, 1, 0,
      .meta.scalar = { 0, 0 },
      .category = UPS_REG_CATEGORY_MEASUREMENT },
    { "output_frequency", "Output Frequency", "Hz", "output",
      0, 1, UPS_CFG_SCALAR, 1, 0,
      .meta.scalar = { 0, 0 },
      .category = UPS_REG_CATEGORY_MEASUREMENT },

    { "nominal_watts", "Nominal Real Power", "W", "info",
      0, 1, UPS_CFG_SCALAR, 1, 0,
      .meta.scalar = { 0, 0 },
      .category = UPS_REG_CATEGORY_IDENTITY },

    /* Operator-tunable delays */
    { "delay_before_shutdown", "Delay Before Shutdown", "s", "delays",
      0, 1, UPS_CFG_SCALAR, 1, 1,
      .meta.scalar = { 0, 0 } },
    { "delay_before_startup", "Delay Before Startup", "s", "delays",
      0, 1, UPS_CFG_SCALAR, 1, 1,
      .meta.scalar = { 0, 0 } },
    { "delay_before_reboot", "Delay Before Reboot", "s", "delays",
      0, 1, UPS_CFG_SCALAR, 1, 1,
      .meta.scalar = { 0, 0 } },
};

#define CYBERPOWER_CFG_COUNT (sizeof(cyberpower_config_regs) / sizeof(cyberpower_config_regs[0]))

enum {
    CFG_TRANSFER_LOW = 0,
    CFG_TRANSFER_HIGH,
    CFG_ALARM_SETTING,
    CFG_LOW_BATTERY_WARNING,
    CFG_LOW_BATTERY_THRESHOLD,
    CFG_WARNING_CAPACITY,
    CFG_MANUFACTURE_DATE,
    CFG_NOMINAL_VOLTAGE,
    CFG_NOMINAL_VA,
    CFG_DESIGN_CAPACITY,
    CFG_INPUT_VOLTAGE,
    CFG_BATTERY_VOLTAGE,
    CFG_REMAINING_CAPACITY,
    CFG_RUNTIME_TO_EMPTY,
    CFG_PERCENT_LOAD,
    CFG_OUTPUT_FREQUENCY,
    CFG_NOMINAL_WATTS,
    CFG_DELAY_BEFORE_SHUTDOWN,
    CFG_DELAY_BEFORE_STARTUP,
    CFG_DELAY_BEFORE_REBOOT,
};

static void cyberpower_fixup_config_regs(const hid_pdc_transport_t *t)
{
    #define FIX(idx, fld) hid_pdc_fixup_config_reg(&cyberpower_config_regs[(idx)], (fld), &t->map)

    FIX(CFG_TRANSFER_LOW,          t->pdc.transfer_low);
    FIX(CFG_TRANSFER_HIGH,         t->pdc.transfer_high);
    FIX(CFG_ALARM_SETTING,         t->pdc.alarm_control);
    FIX(CFG_LOW_BATTERY_WARNING,   t->pdc.remaining_time_limit);
    FIX(CFG_LOW_BATTERY_THRESHOLD, t->pdc.remaining_cap_limit);
    FIX(CFG_WARNING_CAPACITY,      t->pdc.warning_cap_limit);
    FIX(CFG_MANUFACTURE_DATE,      t->pdc.manufacture_date);
    FIX(CFG_NOMINAL_VOLTAGE,       t->pdc.output_voltage);
    FIX(CFG_NOMINAL_VA,            t->pdc.config_apparent_power);
    FIX(CFG_DESIGN_CAPACITY,       t->pdc.design_capacity);
    FIX(CFG_INPUT_VOLTAGE,         t->pdc.input_voltage);
    FIX(CFG_BATTERY_VOLTAGE,       t->pdc.battery_voltage);
    FIX(CFG_REMAINING_CAPACITY,    t->pdc.remaining_capacity);
    FIX(CFG_RUNTIME_TO_EMPTY,      t->pdc.runtime_to_empty);
    FIX(CFG_PERCENT_LOAD,          t->pdc.percent_load);
    FIX(CFG_OUTPUT_FREQUENCY,      t->pdc.output_frequency);
    FIX(CFG_NOMINAL_WATTS,         t->pdc.config_active_power);
    FIX(CFG_DELAY_BEFORE_SHUTDOWN, t->pdc.delay_before_shutdown);
    FIX(CFG_DELAY_BEFORE_STARTUP,  t->pdc.delay_before_startup);
    FIX(CFG_DELAY_BEFORE_REBOOT,   t->pdc.delay_before_reboot);
    #undef FIX
}

static int cyberpower_config_read(void *transport, const ups_config_reg_t *reg,
                                   uint32_t *raw_value, char *str_buf, size_t str_bufsz)
{
    (void)str_buf; (void)str_bufsz;
    hid_pdc_transport_t *t = transport;

    const hid_field_t *field = hid_pdc_field_by_index(&t->map, reg->reg_addr);
    int rc = hid_pdc_config_read_field(t->fd, field, reg, raw_value);
    if (rc != UPS_OK) return rc;

    if (raw_value && strcmp(reg->name, "manufacture_date") == 0)
        *raw_value = hid_pdc_usb_date_to_days_since_2000((uint16_t)*raw_value);

    return UPS_OK;
}

static int cyberpower_config_write(void *transport, const ups_config_reg_t *reg, uint16_t value)
{
    hid_pdc_transport_t *t = transport;

    if (!reg->writable)
        return UPS_ERR_NOT_SUPPORTED;

    const hid_field_t *field = hid_pdc_field_by_index(&t->map, reg->reg_addr);
    return hid_pdc_config_write_field(t->fd, field, reg, value);
}

/* --- Capability + register narrowing ------------------------------------ */

static uint32_t cyberpower_resolve_caps(void *transport, uint32_t default_caps)
{
    hid_pdc_transport_t *t = transport;
    uint32_t caps = default_caps;

    if (!t->pdc.delay_before_shutdown) caps &= (uint32_t)~UPS_CAP_SHUTDOWN;
    if (!t->pdc.test)                  caps &= (uint32_t)~UPS_CAP_BATTERY_TEST;
    if (!t->pdc.module_reset)          caps &= (uint32_t)~UPS_CAP_CLEAR_FAULTS;
    if (!t->pdc.alarm_control)         caps &= (uint32_t)~UPS_CAP_MUTE;

    return caps;
}

static size_t cyberpower_resolve_config_regs(void *transport,
                                              const ups_config_reg_t *default_regs,
                                              size_t default_count,
                                              ups_config_reg_t *out)
{
    hid_pdc_transport_t *t = transport;
    (void)default_count;

    cyberpower_fixup_config_regs(t);

    size_t n = 0;
    #define KEEP_IF(idx, fld) do { if ((fld)) out[n++] = default_regs[(idx)]; } while (0)
    KEEP_IF(CFG_TRANSFER_LOW,             t->pdc.transfer_low);
    KEEP_IF(CFG_TRANSFER_HIGH,            t->pdc.transfer_high);
    KEEP_IF(CFG_ALARM_SETTING,            t->pdc.alarm_control);
    KEEP_IF(CFG_LOW_BATTERY_WARNING,      t->pdc.remaining_time_limit);
    KEEP_IF(CFG_LOW_BATTERY_THRESHOLD,    t->pdc.remaining_cap_limit);
    KEEP_IF(CFG_WARNING_CAPACITY,         t->pdc.warning_cap_limit);
    KEEP_IF(CFG_MANUFACTURE_DATE,         t->pdc.manufacture_date);
    KEEP_IF(CFG_NOMINAL_VOLTAGE,          t->pdc.output_voltage);
    KEEP_IF(CFG_NOMINAL_VA,               t->pdc.config_apparent_power);
    KEEP_IF(CFG_DESIGN_CAPACITY,          t->pdc.design_capacity);
    KEEP_IF(CFG_INPUT_VOLTAGE,            t->pdc.input_voltage);
    KEEP_IF(CFG_BATTERY_VOLTAGE,          t->pdc.battery_voltage);
    KEEP_IF(CFG_REMAINING_CAPACITY,       t->pdc.remaining_capacity);
    KEEP_IF(CFG_RUNTIME_TO_EMPTY,         t->pdc.runtime_to_empty);
    KEEP_IF(CFG_PERCENT_LOAD,             t->pdc.percent_load);
    KEEP_IF(CFG_OUTPUT_FREQUENCY,         t->pdc.output_frequency);
    KEEP_IF(CFG_NOMINAL_WATTS,            t->pdc.config_active_power);
    KEEP_IF(CFG_DELAY_BEFORE_SHUTDOWN,    t->pdc.delay_before_shutdown);
    KEEP_IF(CFG_DELAY_BEFORE_STARTUP,     t->pdc.delay_before_startup);
    KEEP_IF(CFG_DELAY_BEFORE_REBOOT,      t->pdc.delay_before_reboot);
    #undef KEEP_IF
    return n;
}

/* --- Command descriptors ------------------------------------------------- */

static const ups_cmd_desc_t cyberpower_commands[] = {
    { "shutdown", "Shutdown UPS", "Send the UPS shutdown command (60 second delay)",
      "power", "Shutdown UPS?",
      "This sends the shutdown command to the UPS with a 60 second delay.",
      UPS_CMD_SIMPLE, UPS_CMD_DANGER, UPS_CMD_IS_SHUTDOWN, 0,
      cp_cmd_shutdown, NULL },

    { "abort_shutdown", "Abort Shutdown", "Cancel a pending shutdown countdown",
      "power", "Abort Shutdown?",
      "This will cancel any pending shutdown countdown.",
      UPS_CMD_SIMPLE, UPS_CMD_DEFAULT, 0, 0,
      cp_cmd_abort_shutdown, NULL },

    { "battery_test", "Battery Test", "Run a quick battery self-test",
      "diagnostics", "Start Battery Test?",
      "This will run a brief self-test to verify battery health.",
      UPS_CMD_SIMPLE, UPS_CMD_DEFAULT, 0, 0,
      cp_cmd_battery_test, NULL },

    { "clear_faults", "Clear Faults", "Reset latched fault flags",
      "diagnostics", "Clear Faults?",
      "This will reset any latched alarm or fault indicators.",
      UPS_CMD_SIMPLE, UPS_CMD_DEFAULT, 0, 0,
      cp_cmd_clear_faults, NULL },

    { "mute", "Mute Alarm", "Silence the UPS audible alarm",
      "alarm", "Mute Alarm?",
      "This will silence the UPS audible alarm.",
      UPS_CMD_SIMPLE, UPS_CMD_DEFAULT, UPS_CMD_IS_MUTE, 0,
      cp_cmd_mute, NULL },

    { "unmute", "Unmute Alarm", "Re-enable the UPS audible alarm",
      "alarm", "Unmute Alarm?",
      "This will re-enable the UPS audible alarm.",
      UPS_CMD_SIMPLE, UPS_CMD_DEFAULT, 0, 0,
      cp_cmd_unmute, NULL },
};

/* --- Driver definition --------------------------------------------------- */

const ups_driver_t ups_driver_cyberpower_hid = {
    .name                = "cyberpower_hid",
    .conn_type           = UPS_CONN_USB,
    .topology            = UPS_TOPO_LINE_INTERACTIVE,  /* default; overridden by get_topology */
    .connect             = cyberpower_connect,
    .disconnect          = cyberpower_disconnect,
    .detect              = cyberpower_detect,
    .get_topology        = cyberpower_get_topology,
    .caps                = UPS_CAP_SHUTDOWN | UPS_CAP_BATTERY_TEST |
                           UPS_CAP_CLEAR_FAULTS | UPS_CAP_MUTE,
    .resolve_caps        = cyberpower_resolve_caps,
    .freq_settings       = NULL,
    .freq_settings_count = 0,
    .config_regs         = cyberpower_config_regs,
    .config_regs_count   = CYBERPOWER_CFG_COUNT,
    .resolve_config_regs = cyberpower_resolve_config_regs,
    .read_status         = cyberpower_read_status,
    .read_dynamic        = cyberpower_read_dynamic,
    .read_inventory      = cyberpower_read_inventory,
    .read_thresholds     = cyberpower_read_thresholds,
    .commands            = cyberpower_commands,
    .commands_count      = sizeof(cyberpower_commands) / sizeof(cyberpower_commands[0]),
    .config_read         = cyberpower_config_read,
    .config_write        = cyberpower_config_write,
};
