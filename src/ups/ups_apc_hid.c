#include "hid_pdc_core.h"
#include "ups.h"
#include "ups_driver.h"

#include <cutils/log.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- APC HID Power Device driver ---
 *
 * Covers the APC Back-UPS family (BE, BR, BX, BN series) and any other
 * APC HID-PDC devices that show up at VID 0x051D PID 0x0002. Uses the
 * vendor-neutral hid_pdc_core for descriptor parsing, standard PDC field
 * resolution, status/dynamic reads, and standard commands. This file
 * carries only the APC-specific layer:
 *
 *   - APC vendor page (0xFF86) field declarations and resolution
 *   - APC-only command: lights/beeper test
 *   - APC-only config registers: sensitivity, batt replacement date,
 *     startup capacity
 *   - Detection by USB manufacturer/product strings
 *   - Nameplate VA/W lookup table for SKUs whose descriptor lacks the
 *     ConfigApparentPower / ConfigActivePower fields. */

#define APC_VID 0x051D
#define APC_PID 0x0002

/* APC vendor page (0xFF86) usages — from AN178 + descriptor analysis.
 * These are APC-specific and do NOT belong in the standard PDC header. */
#define APC_PAGE                    0xFF86
#define APC_USAGE_SENSITIVITY       0x0061
#define APC_USAGE_BATT_REPL_DATE    0x0024
#define APC_USAGE_MANUF_DATE        0x0025
#define APC_USAGE_BATT_CAP_STARTUP  0x0026
#define APC_USAGE_LIGHTS_TEST       0x0072

static const uint32_t UP_APC_SENSITIVITY[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_INPUT),
    HID_UP(APC_PAGE, APC_USAGE_SENSITIVITY) };

static const uint32_t UP_APC_LIGHTS_TEST[] = {
    HID_UP(APC_PAGE, APC_USAGE_LIGHTS_TEST) };

static const uint32_t UP_APC_BATT_REPL_DATE[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_BATTERY),
    HID_UP(APC_PAGE, APC_USAGE_BATT_REPL_DATE) };

static const uint32_t UP_APC_MANUF_DATE[] = {
    HID_UP(APC_PAGE, APC_USAGE_MANUF_DATE) };

static const uint32_t UP_APC_BATT_CAP_STARTUP[] = {
    HID_UP(APC_PAGE, APC_USAGE_BATT_CAP_STARTUP) };

/* --- APC vendor field cache (stashed on transport->vendor) --------------- */

typedef struct {
    const hid_field_t *sensitivity;
    const hid_field_t *lights_test;
    const hid_field_t *batt_repl_date;
    const hid_field_t *manuf_date;
    const hid_field_t *batt_cap_startup;
} apc_vendor_t;

/* --- Known APC nameplate ratings (fallback when descriptor lacks them) -- */

typedef struct {
    const char *model;
    uint16_t    va;
    uint16_t    watts;
} apc_model_spec_t;

static const apc_model_spec_t apc_known_models[] = {
    { "BE600M1",     600,  330 },
    { "ES 600M1",    600,  330 },
    { "RS 1500MS2", 1500,  900 },
};

/* --- Lifecycle ----------------------------------------------------------- */

static void *apc_connect(const ups_conn_params_t *params)
{
    if (!params || params->type != UPS_CONN_USB) return NULL;

    /* Precheck VID/PID before opening hidraw — keeps the registry probe
     * cheap when the user has a non-APC device configured. */
    if (params->usb.vendor_id != APC_VID || params->usb.product_id != APC_PID)
        return NULL;

    hid_pdc_transport_t *t = hid_pdc_open(params);
    if (!t) return NULL;

    apc_vendor_t *v = calloc(1, sizeof(*v));
    if (!v) { hid_pdc_close(t); return NULL; }

    v->sensitivity      = hid_find_field(&t->map, UP_APC_SENSITIVITY,      2, HID_FIELD_FEATURE);
    v->lights_test      = hid_find_field(&t->map, UP_APC_LIGHTS_TEST,      1, HID_FIELD_FEATURE);
    v->batt_repl_date   = hid_find_field(&t->map, UP_APC_BATT_REPL_DATE,   2, HID_FIELD_FEATURE);
    v->manuf_date       = hid_find_field(&t->map, UP_APC_MANUF_DATE,       1, HID_FIELD_FEATURE);
    v->batt_cap_startup = hid_find_field(&t->map, UP_APC_BATT_CAP_STARTUP, 1, HID_FIELD_FEATURE);

    struct { const char *name; const hid_field_t *field; } vfields[] = {
        { "apc_sensitivity",      v->sensitivity },
        { "apc_lights_test",      v->lights_test },
        { "apc_batt_repl_date",   v->batt_repl_date },
        { "apc_manuf_date",       v->manuf_date },
        { "apc_batt_cap_startup", v->batt_cap_startup },
    };
    for (size_t i = 0; i < sizeof(vfields) / sizeof(vfields[0]); i++) {
        if (vfields[i].field)
            log_info("apc_hid: %s -> RID 0x%02x", vfields[i].name,
                     vfields[i].field->report_id);
    }

    t->vendor = v;
    return t;
}

static void apc_disconnect(void *transport)
{
    hid_pdc_transport_t *t = transport;
    if (!t) return;
    free(t->vendor);
    hid_pdc_close(t);
}

static int apc_detect(void *transport)
{
    hid_pdc_transport_t *t = transport;

    /* Warm-up read on remaining_capacity to wake the device, mirroring
     * the original Back-UPS behaviour. Some units return 0 on the first
     * read after open and the real value on the second. */
    if (t->pdc.remaining_capacity) {
        int32_t cap = hid_field_read_raw(t->fd, t->pdc.remaining_capacity);
        if (cap == 0)
            cap = hid_field_read_raw(t->fd, t->pdc.remaining_capacity);
        (void)cap;
    }

    char product[128];
    hid_pdc_read_sysfs_string(t->sysfs_base, "product", product, sizeof(product));
    if (strstr(product, "Back-UPS") != NULL)
        return 1;

    char mfg[128];
    hid_pdc_read_sysfs_string(t->sysfs_base, "manufacturer", mfg, sizeof(mfg));
    return strstr(mfg, "American Power Conversion") != NULL;
}

static ups_topology_t apc_get_topology(void *transport)
{
    hid_pdc_transport_t *t = transport;
    return t->topology;
}

/* --- Reads --------------------------------------------------------------- */

static int apc_read_status(void *transport, ups_data_t *data)
{
    return hid_pdc_read_status_standard(transport, data);
}

static int apc_read_dynamic(void *transport, ups_data_t *data)
{
    return hid_pdc_read_dynamic_standard(transport, data);
}

static int apc_read_inventory(void *transport, ups_inventory_t *inv)
{
    hid_pdc_transport_t *t = transport;
    memset(inv, 0, sizeof(*inv));

    hid_pdc_read_sysfs_string(t->sysfs_base, "product", inv->model, sizeof(inv->model));
    hid_pdc_read_sysfs_string(t->sysfs_base, "serial", inv->serial, sizeof(inv->serial));

    /* APC encodes firmware revision as "FW:..." inside the product string. */
    char *fw = strstr(inv->model, "FW:");
    if (fw)
        snprintf(inv->firmware, sizeof(inv->firmware), "%s", fw);

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
        log_info("apc_hid: VA from descriptor — %u VA", inv->nominal_va);
    if (inv->nominal_watts > 0)
        log_info("apc_hid: watts from descriptor — %u W", inv->nominal_watts);

    if (inv->nominal_va == 0 || inv->nominal_watts == 0) {
        for (size_t i = 0; i < sizeof(apc_known_models) / sizeof(apc_known_models[0]); i++) {
            if (strstr(inv->model, apc_known_models[i].model)) {
                if (inv->nominal_va == 0) {
                    inv->nominal_va = apc_known_models[i].va;
                    log_info("apc_hid: VA from model lookup '%s' — %u VA",
                             apc_known_models[i].model, inv->nominal_va);
                }
                if (inv->nominal_watts == 0) {
                    inv->nominal_watts = apc_known_models[i].watts;
                    log_info("apc_hid: watts from model lookup '%s' — %u W",
                             apc_known_models[i].model, inv->nominal_watts);
                }
                break;
            }
        }
    }

    if (inv->nominal_va == 0)
        log_warn("apc_hid: could not determine VA rating");
    if (inv->nominal_watts == 0)
        log_warn("apc_hid: could not determine watt rating");

    inv->sog_config    = 0;
    t->nominal_watts   = inv->nominal_watts;
    return 0;
}

static int apc_read_thresholds(void *transport,
                                uint16_t *transfer_high,
                                uint16_t *transfer_low)
{
    return hid_pdc_read_thresholds_standard(transport, transfer_high, transfer_low);
}

/* --- Commands ------------------------------------------------------------ */

static int apc_cmd_shutdown(void *transport)      { return hid_pdc_cmd_shutdown(transport); }
static int apc_cmd_abort_shutdown(void *transport){ return hid_pdc_cmd_abort_shutdown(transport); }
static int apc_cmd_battery_test(void *transport)  { return hid_pdc_cmd_battery_test(transport); }
static int apc_cmd_mute(void *transport)          { return hid_pdc_cmd_mute_alarm(transport); }
static int apc_cmd_unmute(void *transport)        { return hid_pdc_cmd_unmute_alarm(transport); }
static int apc_cmd_clear_faults(void *transport)  { return hid_pdc_cmd_clear_faults(transport); }

static int apc_cmd_beep_test(void *transport)
{
    hid_pdc_transport_t *t = transport;
    apc_vendor_t *v = t->vendor;
    if (!v || !v->lights_test) return -1;
    /* APCLightsAndBeeperTest (FF86:72): write 1 to execute. */
    uint8_t buf[2] = { v->lights_test->report_id, 0x01 };
    return hid_pdc_set_feature(t->fd, buf[0], buf, sizeof(buf)) >= 0 ? 0 : -1;
}

/* --- Config registers ---------------------------------------------------- */

static const ups_bitfield_opt_t sensitivity_opts[] = {
    { 0, "low",    "Low" },
    { 1, "medium", "Medium" },
    { 2, "high",   "High" },
};

static const ups_bitfield_opt_t alarm_opts[] = {
    { 1, "disabled", "Disabled" },
    { 2, "enabled",  "Enabled" },
    { 3, "muted",    "Muted" },
};

/* Initial reg_addr values are placeholders (0); apc_fixup_config_regs()
 * fills them in with the resolved HID report IDs at connect time. */
static ups_config_reg_t apc_config_regs[] = {
    /* --- Group: transfer --- */
    { "transfer_low", "Low Transfer Voltage", "V", "transfer",
      0, 1, UPS_CFG_SCALAR, 1, 1,
      .meta.scalar = { 0, 0 } },
    { "transfer_high", "High Transfer Voltage", "V", "transfer",
      0, 1, UPS_CFG_SCALAR, 1, 1,
      .meta.scalar = { 0, 0 } },

    /* --- Group: power_quality (APC vendor) --- */
    { "sensitivity", "Input Sensitivity", NULL, "power_quality",
      0, 1, UPS_CFG_BITFIELD, 1, 1,
      .meta.bitfield = { sensitivity_opts,
                         sizeof(sensitivity_opts) / sizeof(sensitivity_opts[0]),
                         1 } },

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

    /* --- Live measurements (parity with /api/about register dump) --- */
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

    /* APC vendor extensions */
    { "battery_replacement_date", "Battery Replacement Date", "days since 2000-01-01", "battery",
      0, 1, UPS_CFG_SCALAR, 1, 1,
      .meta.scalar = { 0, 0 } },
    { "battery_capacity_for_startup", "Battery Capacity Required for Startup", "%", "battery",
      0, 1, UPS_CFG_SCALAR, 1, 1,
      .meta.scalar = { 0, 0 } },
};

#define APC_CFG_COUNT (sizeof(apc_config_regs) / sizeof(apc_config_regs[0]))

enum {
    CFG_TRANSFER_LOW = 0,
    CFG_TRANSFER_HIGH,
    CFG_SENSITIVITY,
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
    CFG_BATTERY_REPLACEMENT_DATE,
    CFG_BATTERY_CAPACITY_FOR_STARTUP,
};

static void apc_fixup_config_regs(const hid_pdc_transport_t *t)
{
    const apc_vendor_t *v = t->vendor;
    #define FIX(idx, fld) hid_pdc_fixup_config_reg(&apc_config_regs[(idx)], (fld), &t->map)

    FIX(CFG_TRANSFER_LOW,          t->pdc.transfer_low);
    FIX(CFG_TRANSFER_HIGH,         t->pdc.transfer_high);
    FIX(CFG_SENSITIVITY,           v->sensitivity);
    FIX(CFG_ALARM_SETTING,         t->pdc.alarm_control);
    FIX(CFG_LOW_BATTERY_WARNING,   t->pdc.remaining_time_limit);
    FIX(CFG_LOW_BATTERY_THRESHOLD, t->pdc.remaining_cap_limit);
    FIX(CFG_WARNING_CAPACITY,      t->pdc.warning_cap_limit);
    FIX(CFG_MANUFACTURE_DATE,      t->pdc.manufacture_date);
    FIX(CFG_NOMINAL_VOLTAGE,       t->pdc.output_voltage);
    FIX(CFG_NOMINAL_VA,            t->pdc.config_apparent_power);
    FIX(CFG_DESIGN_CAPACITY,       t->pdc.design_capacity);

    FIX(CFG_INPUT_VOLTAGE,             t->pdc.input_voltage);
    FIX(CFG_BATTERY_VOLTAGE,           t->pdc.battery_voltage);
    FIX(CFG_REMAINING_CAPACITY,        t->pdc.remaining_capacity);
    FIX(CFG_RUNTIME_TO_EMPTY,          t->pdc.runtime_to_empty);
    FIX(CFG_PERCENT_LOAD,              t->pdc.percent_load);
    FIX(CFG_OUTPUT_FREQUENCY,          t->pdc.output_frequency);
    FIX(CFG_NOMINAL_WATTS,             t->pdc.config_active_power);
    FIX(CFG_DELAY_BEFORE_SHUTDOWN,     t->pdc.delay_before_shutdown);
    FIX(CFG_DELAY_BEFORE_STARTUP,      t->pdc.delay_before_startup);
    FIX(CFG_DELAY_BEFORE_REBOOT,       t->pdc.delay_before_reboot);
    FIX(CFG_BATTERY_REPLACEMENT_DATE,  v->batt_repl_date);
    FIX(CFG_BATTERY_CAPACITY_FOR_STARTUP, v->batt_cap_startup);
    #undef FIX
}

static int apc_config_read(void *transport, const ups_config_reg_t *reg,
                            uint32_t *raw_value, char *str_buf, size_t str_bufsz)
{
    (void)str_buf; (void)str_bufsz;
    hid_pdc_transport_t *t = transport;

    const hid_field_t *field = hid_pdc_field_by_index(&t->map, reg->reg_addr);
    int rc = hid_pdc_config_read_field(t->fd, field, reg, raw_value);
    if (rc != UPS_OK) return rc;

    /* Convert standard PDC battery date encoding so the API's date
     * formatter handles it identically to other drivers. */
    if (raw_value && strcmp(reg->name, "manufacture_date") == 0)
        *raw_value = hid_pdc_usb_date_to_days_since_2000((uint16_t)*raw_value);

    return UPS_OK;
}

static int apc_config_write(void *transport, const ups_config_reg_t *reg, uint16_t value)
{
    hid_pdc_transport_t *t = transport;

    if (!reg->writable)
        return UPS_ERR_NOT_SUPPORTED;

    const hid_field_t *field = hid_pdc_field_by_index(&t->map, reg->reg_addr);
    return hid_pdc_config_write_field(t->fd, field, reg, value);
}

/* --- Capability + register narrowing ------------------------------------ */

static uint32_t apc_resolve_caps(void *transport, uint32_t default_caps)
{
    hid_pdc_transport_t *t = transport;
    const apc_vendor_t *v = t->vendor;
    uint32_t caps = default_caps;

    if (!t->pdc.delay_before_shutdown) caps &= (uint32_t)~UPS_CAP_SHUTDOWN;
    if (!t->pdc.test)                  caps &= (uint32_t)~UPS_CAP_BATTERY_TEST;
    if (!t->pdc.module_reset)          caps &= (uint32_t)~UPS_CAP_CLEAR_FAULTS;
    if (!t->pdc.alarm_control)         caps &= (uint32_t)~UPS_CAP_MUTE;
    if (!v || !v->lights_test)         caps &= (uint32_t)~UPS_CAP_BEEP;

    return caps;
}

/* Drop config descriptors whose backing HID field didn't resolve so the
 * frontend only sees settings that actually work on this SKU. */
static size_t apc_resolve_config_regs(void *transport,
                                       const ups_config_reg_t *default_regs,
                                       size_t default_count,
                                       ups_config_reg_t *out)
{
    hid_pdc_transport_t *t = transport;
    const apc_vendor_t *v = t->vendor;
    (void)default_count;

    /* Run fixup_config_regs from the resolve hook so it executes on every
     * connect AND reconnect — the registry calls resolve_config_regs in
     * both paths but doesn't separately re-invoke a per-driver init. */
    apc_fixup_config_regs(t);

    size_t n = 0;
    #define KEEP_IF(idx, fld) do { if ((fld)) out[n++] = default_regs[(idx)]; } while (0)
    KEEP_IF(CFG_TRANSFER_LOW,             t->pdc.transfer_low);
    KEEP_IF(CFG_TRANSFER_HIGH,            t->pdc.transfer_high);
    KEEP_IF(CFG_SENSITIVITY,              v ? v->sensitivity : NULL);
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
    KEEP_IF(CFG_BATTERY_REPLACEMENT_DATE, v ? v->batt_repl_date   : NULL);
    KEEP_IF(CFG_BATTERY_CAPACITY_FOR_STARTUP, v ? v->batt_cap_startup : NULL);
    #undef KEEP_IF
    return n;
}

/* --- Command descriptors ------------------------------------------------- */

static const ups_cmd_desc_t apc_commands[] = {
    { "shutdown", "Shutdown UPS", "Send the UPS shutdown command (60 second delay)",
      "power", "Shutdown UPS?",
      "This sends the shutdown command to the UPS with a 60 second delay.",
      UPS_CMD_SIMPLE, UPS_CMD_DANGER, UPS_CMD_IS_SHUTDOWN, 0,
      apc_cmd_shutdown, NULL },

    { "abort_shutdown", "Abort Shutdown", "Cancel a pending shutdown countdown",
      "power", "Abort Shutdown?",
      "This will cancel any pending shutdown countdown.",
      UPS_CMD_SIMPLE, UPS_CMD_DEFAULT, 0, 0,
      apc_cmd_abort_shutdown, NULL },

    { "battery_test", "Battery Test", "Run a quick battery self-test",
      "diagnostics", "Start Battery Test?",
      "This will run a brief self-test to verify battery health.",
      UPS_CMD_SIMPLE, UPS_CMD_DEFAULT, 0, 0,
      apc_cmd_battery_test, NULL },

    { "clear_faults", "Clear Faults", "Reset latched fault flags",
      "diagnostics", "Clear Faults?",
      "This will reset any latched alarm or fault indicators.",
      UPS_CMD_SIMPLE, UPS_CMD_DEFAULT, 0, 0,
      apc_cmd_clear_faults, NULL },

    { "beep_short", "Beep / LED Test", "Test the audible alarm and panel LEDs",
      "alarm", "Run Beep Test?",
      "This will briefly activate the audible alarm and panel LEDs.",
      UPS_CMD_SIMPLE, UPS_CMD_DEFAULT, 0, 0,
      apc_cmd_beep_test, NULL },

    { "mute", "Mute Alarm", "Silence the UPS audible alarm",
      "alarm", "Mute Alarm?",
      "This will silence the UPS audible alarm.",
      UPS_CMD_SIMPLE, UPS_CMD_DEFAULT, UPS_CMD_IS_MUTE, 0,
      apc_cmd_mute, NULL },

    { "unmute", "Unmute Alarm", "Re-enable the UPS audible alarm",
      "alarm", "Unmute Alarm?",
      "This will re-enable the UPS audible alarm.",
      UPS_CMD_SIMPLE, UPS_CMD_DEFAULT, 0, 0,
      apc_cmd_unmute, NULL },
};

/* --- Driver definition --------------------------------------------------- */

const ups_driver_t ups_driver_apc_hid = {
    .name                = "apc_hid",
    .conn_type           = UPS_CONN_USB,
    .topology            = UPS_TOPO_STANDBY,  /* default; overridden by get_topology */
    .connect             = apc_connect,
    .disconnect          = apc_disconnect,
    .detect              = apc_detect,
    .get_topology        = apc_get_topology,
    /* Maximum advertised caps; apc_resolve_caps narrows to what actually
     * resolved against the live device's HID descriptor. */
    .caps                = UPS_CAP_SHUTDOWN | UPS_CAP_BATTERY_TEST |
                           UPS_CAP_CLEAR_FAULTS |
                           UPS_CAP_MUTE | UPS_CAP_BEEP,
    .resolve_caps        = apc_resolve_caps,
    .freq_settings       = NULL,
    .freq_settings_count = 0,
    .config_regs         = apc_config_regs,
    .config_regs_count   = APC_CFG_COUNT,
    .resolve_config_regs = apc_resolve_config_regs,
    .read_status         = apc_read_status,
    .read_dynamic        = apc_read_dynamic,
    .read_inventory      = apc_read_inventory,
    .read_thresholds     = apc_read_thresholds,
    .commands            = apc_commands,
    .commands_count      = sizeof(apc_commands) / sizeof(apc_commands[0]),
    .config_read         = apc_config_read,
    .config_write        = apc_config_write,
};
