/* realpath() requires _XOPEN_SOURCE >= 500 on some toolchains */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 500
#endif

#include "ups_driver.h"
#include "hid_parser.h"
#include "ups.h"
#include <cutils/log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <limits.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/hidraw.h>

/* --- APC Back-UPS HID driver ---
 *
 * Communicates via USB HID feature reports over /dev/hidrawN.
 * Implements the USB HID Power Device Class spec (pages 0x84, 0x85)
 * with APC vendor extensions (page 0xFF86).
 *
 * All report IDs and unit scaling are derived from parsing the HID
 * report descriptor at connect time — no hardcoded report IDs.
 *
 * Covers the APC Back-UPS product family (BE, BR, BX, BN series).
 * Topology is detected from the descriptor: standby (no PowerConverter)
 * or line-interactive (PowerConverter collection present). */

#define APC_VID  0x051D
#define APC_PID  0x0002

/* --- APC vendor page (0xFF86) ---
 * Usages from AN178 and descriptor analysis. These are APC-specific
 * and do NOT belong in the generic hid_parser.h header. */
#define APC_PAGE                    0xFF86
#define APC_USAGE_STATUS_CODE       0x0001
#define APC_USAGE_SENSITIVITY       0x0061
#define APC_USAGE_PANELTEST         0x0018
#define APC_USAGE_STATUS_BYTE       0x0023
#define APC_USAGE_BATT_REPL_DATE    0x0024
#define APC_USAGE_MANUF_DATE        0x0025
#define APC_USAGE_BATT_CAP_STARTUP  0x0026
#define APC_USAGE_FW_REVISION       0x0042
#define APC_USAGE_SHUTDOWN_AFTER    0x0060
#define APC_USAGE_LIGHTS_TEST       0x0072

/* --- Usage paths for field lookup ---
 * Collection-qualified: [collection..., usage] matched as a suffix
 * against the full path in the parsed descriptor. */

/* Power.Input collection */
static const uint32_t UP_INPUT_VOLTAGE[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_INPUT),
    HID_UP(HID_PAGE_POWER, HID_USAGE_VOLTAGE) };

static const uint32_t UP_INPUT_CONFIG_VOLTAGE[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_INPUT),
    HID_UP(HID_PAGE_POWER, HID_USAGE_CONFIG_VOLTAGE) };

static const uint32_t UP_TRANSFER_LOW[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_INPUT),
    HID_UP(HID_PAGE_POWER, HID_USAGE_LOW_VOLTAGE_TRANSFER) };

static const uint32_t UP_TRANSFER_HIGH[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_INPUT),
    HID_UP(HID_PAGE_POWER, HID_USAGE_HIGH_VOLTAGE_TRANSFER) };

/* Power.Battery collection */
static const uint32_t UP_BATTERY_VOLTAGE[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_BATTERY),
    HID_UP(HID_PAGE_POWER, HID_USAGE_VOLTAGE) };

static const uint32_t UP_REMAINING_CAPACITY[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_BATTERY),
    HID_UP(HID_PAGE_BATTERY, HID_USAGE_BAT_REMAINING_CAPACITY) };

static const uint32_t UP_RUNTIME_TO_EMPTY[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_BATTERY),
    HID_UP(HID_PAGE_BATTERY, HID_USAGE_BAT_RUNTIME_TO_EMPTY) };

/* Power.PowerConverter collection */
static const uint32_t UP_PERCENT_LOAD[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_PERCENT_LOAD) };

/* Power controls — single-usage lookup */
static const uint32_t UP_TEST[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_TEST) };

static const uint32_t UP_ALARM_CONTROL[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_AUDIBLE_ALARM_CTRL) };

static const uint32_t UP_DELAY_SHUTDOWN[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_DELAY_BEFORE_SHUTDOWN) };

static const uint32_t UP_DELAY_STARTUP[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_DELAY_BEFORE_STARTUP) };

static const uint32_t UP_DELAY_REBOOT[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_DELAY_BEFORE_REBOOT) };

static const uint32_t UP_MODULE_RESET[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_MODULE_RESET) };

/* Power.Output.Frequency */
static const uint32_t UP_OUTPUT_FREQUENCY[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_OUTPUT),
    HID_UP(HID_PAGE_POWER, HID_USAGE_FREQUENCY) };

/* Power rating — ConfigApparentPower (VA) and ConfigActivePower (W) */
static const uint32_t UP_CONFIG_APPARENT_POWER[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_CONFIG_APPARENT_POWER) };

static const uint32_t UP_CONFIG_ACTIVE_POWER[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_CONFIG_ACTIVE_POWER) };

/* Battery settings and info */
static const uint32_t UP_REMAINING_TIME_LIMIT[] = {
    HID_UP(HID_PAGE_BATTERY, HID_USAGE_BAT_REMAINING_TIME_LIM) };

static const uint32_t UP_REMAINING_CAP_LIMIT[] = {
    HID_UP(HID_PAGE_BATTERY, HID_USAGE_BAT_REMAINING_CAP_LIM) };

static const uint32_t UP_WARNING_CAP_LIMIT[] = {
    HID_UP(HID_PAGE_BATTERY, HID_USAGE_BAT_WARNING_CAP_LIMIT) };

static const uint32_t UP_DESIGN_CAPACITY[] = {
    HID_UP(HID_PAGE_BATTERY, HID_USAGE_BAT_DESIGN_CAPACITY) };

static const uint32_t UP_MANUFACTURE_DATE[] = {
    HID_UP(HID_PAGE_BATTERY, HID_USAGE_BAT_MANUFACTURE_DATE) };

/* APC vendor paths */
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

/* --- Known model lookup table (fallback when descriptor lacks power fields) --- */

typedef struct {
    const char *model;
    uint16_t    va;
    uint16_t    watts;
} backups_model_spec_t;

static const backups_model_spec_t known_models[] = {
    { "BE600M1",     600,  330 },
    { "ES 600M1",    600,  330 },
    { "RS 1500MS2", 1500,  900 },
};

/* --- Resolved field cache (populated on connect) --- */

typedef struct {
    /* Measures */
    const hid_field_t *input_voltage;
    const hid_field_t *output_voltage;        /* Input.ConfigVoltage (nominal) */
    const hid_field_t *battery_voltage;
    const hid_field_t *remaining_capacity;
    const hid_field_t *runtime_to_empty;
    const hid_field_t *percent_load;
    const hid_field_t *output_frequency;

    /* Config / thresholds */
    const hid_field_t *transfer_low;
    const hid_field_t *transfer_high;
    const hid_field_t *config_apparent_power;
    const hid_field_t *config_active_power;

    /* Controls */
    const hid_field_t *test;
    const hid_field_t *alarm_control;
    const hid_field_t *delay_before_shutdown;
    const hid_field_t *delay_before_startup;
    const hid_field_t *delay_before_reboot;
    const hid_field_t *module_reset;

    /* APC vendor */
    const hid_field_t *apc_sensitivity;
    const hid_field_t *apc_lights_test;
    const hid_field_t *apc_batt_repl_date;
    const hid_field_t *apc_manuf_date;
    const hid_field_t *apc_batt_cap_startup;

    /* Battery settings */
    const hid_field_t *remaining_time_limit;
    const hid_field_t *remaining_cap_limit;
    const hid_field_t *warning_cap_limit;
    const hid_field_t *design_capacity;
    const hid_field_t *manufacture_date;
} hid_fields_t;

/* --- Transport --- */

typedef struct {
    int              fd;
    char             sysfs_base[PATH_MAX];
    char             hidraw_name[NAME_MAX];
    hid_report_map_t map;
    hid_fields_t     f;
    uint16_t         nominal_watts;
    ups_topology_t   topology;
} hid_transport_t;

/* --- hidraw helpers --- */

static int hid_set_feature(int fd, uint8_t report_id, const void *buf, size_t len)
{
    (void)report_id;
    return ioctl(fd, HIDIOCSFEATURE(len), buf);
}

/* --- Find hidraw device by vendor/product --- */

static int find_hidraw_device(uint16_t vid, uint16_t pid,
                              char *path, size_t path_sz,
                              char *sysfs_base, size_t sysfs_sz,
                              char hidraw_name[static NAME_MAX])
{
    DIR *dir = opendir("/sys/class/hidraw");
    if (!dir) return -1;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char uevent[PATH_MAX];
        snprintf(uevent, sizeof(uevent),
                 "/sys/class/hidraw/%s/device/uevent", ent->d_name);

        FILE *f = fopen(uevent, "r");
        if (!f) continue;

        char line[256];
        int match = 0;
        while (fgets(line, sizeof(line), f)) {
            unsigned int hid_bus, hid_vid, hid_pid;
            if (sscanf(line, "HID_ID=%x:%x:%x", &hid_bus, &hid_vid, &hid_pid) == 3) {
                if (hid_vid == vid && hid_pid == pid)
                    match = 1;
            }
        }
        fclose(f);

        if (match) {
            snprintf(path, path_sz, "/dev/%s", ent->d_name);
            memcpy(hidraw_name, ent->d_name, strlen(ent->d_name) + 1);

            if (sysfs_base && sysfs_sz > 0) {
                char devlink[PATH_MAX];
                snprintf(devlink, sizeof(devlink),
                         "/sys/class/hidraw/%s/device", ent->d_name);
                char resolved[PATH_MAX];
                if (realpath(devlink, resolved)) {
                    for (;;) {
                        char check[PATH_MAX + 16];
                        snprintf(check, sizeof(check), "%s/idVendor", resolved);
                        if (access(check, R_OK) == 0) {
                            snprintf(sysfs_base, sysfs_sz, "%s", resolved);
                            break;
                        }
                        char *slash = strrchr(resolved, '/');
                        if (!slash || slash == resolved) break;
                        *slash = '\0';
                    }
                }
            }

            closedir(dir);
            return 0;
        }
    }

    closedir(dir);
    return -1;
}

static void read_sysfs_string(const char *base, const char *attr,
                              char *buf, size_t bufsz)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", base, attr);
    FILE *f = fopen(path, "r");
    if (!f) { buf[0] = '\0'; return; }
    if (!fgets(buf, (int)bufsz, f))
        buf[0] = '\0';
    fclose(f);
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
        buf[--len] = '\0';
}

/* Read the HID report descriptor from sysfs and parse it */
static int parse_hid_descriptor(hid_transport_t *t)
{
    char desc_path[PATH_MAX];
    snprintf(desc_path, sizeof(desc_path),
             "/sys/class/hidraw/%s/device/report_descriptor", t->hidraw_name);

    int fd = open(desc_path, O_RDONLY);
    if (fd < 0) {
        log_warn("backups_hid: cannot read descriptor from %s", desc_path);
        return -1;
    }

    uint8_t desc[4096];
    ssize_t dlen = read(fd, desc, sizeof(desc));
    close(fd);

    if (dlen <= 0) {
        log_warn("backups_hid: empty descriptor");
        return -1;
    }

    if (hid_parse_descriptor(desc, (size_t)dlen, &t->map) != 0) {
        log_warn("backups_hid: descriptor parse failed");
        return -1;
    }

    log_info("backups_hid: parsed %zu fields from HID descriptor", t->map.count);
    return 0;
}

/* Resolve all usage paths into cached field pointers */
static void resolve_fields(hid_transport_t *t)
{
    hid_fields_t *f = &t->f;
    const hid_report_map_t *m = &t->map;

    /* Measures */
    f->input_voltage     = hid_find_field(m, UP_INPUT_VOLTAGE, 2, HID_FIELD_FEATURE);
    f->output_voltage    = hid_find_field(m, UP_INPUT_CONFIG_VOLTAGE, 2, HID_FIELD_FEATURE);
    f->battery_voltage   = hid_find_field(m, UP_BATTERY_VOLTAGE, 2, HID_FIELD_FEATURE);
    f->remaining_capacity = hid_find_field(m, UP_REMAINING_CAPACITY, 2, HID_FIELD_FEATURE);
    f->runtime_to_empty  = hid_find_field(m, UP_RUNTIME_TO_EMPTY, 2, HID_FIELD_FEATURE);
    f->percent_load      = hid_find_field(m, UP_PERCENT_LOAD, 1, HID_FIELD_FEATURE);
    f->output_frequency  = hid_find_field(m, UP_OUTPUT_FREQUENCY, 2, HID_FIELD_FEATURE);

    /* Config / thresholds */
    f->transfer_low      = hid_find_field(m, UP_TRANSFER_LOW, 2, HID_FIELD_FEATURE);
    f->transfer_high     = hid_find_field(m, UP_TRANSFER_HIGH, 2, HID_FIELD_FEATURE);
    f->config_apparent_power = hid_find_field(m, UP_CONFIG_APPARENT_POWER, 1, HID_FIELD_FEATURE);
    f->config_active_power   = hid_find_field(m, UP_CONFIG_ACTIVE_POWER, 1, HID_FIELD_FEATURE);

    /* Controls */
    f->test              = hid_find_field(m, UP_TEST, 1, HID_FIELD_FEATURE);
    f->alarm_control     = hid_find_field(m, UP_ALARM_CONTROL, 1, HID_FIELD_FEATURE);
    f->delay_before_shutdown = hid_find_field(m, UP_DELAY_SHUTDOWN, 1, HID_FIELD_FEATURE);
    f->delay_before_startup  = hid_find_field(m, UP_DELAY_STARTUP, 1, HID_FIELD_FEATURE);
    f->delay_before_reboot   = hid_find_field(m, UP_DELAY_REBOOT, 1, HID_FIELD_FEATURE);
    f->module_reset      = hid_find_field(m, UP_MODULE_RESET, 1, HID_FIELD_FEATURE);

    /* APC vendor */
    f->apc_sensitivity   = hid_find_field(m, UP_APC_SENSITIVITY, 2, HID_FIELD_FEATURE);
    f->apc_lights_test   = hid_find_field(m, UP_APC_LIGHTS_TEST, 1, HID_FIELD_FEATURE);
    f->apc_batt_repl_date = hid_find_field(m, UP_APC_BATT_REPL_DATE, 2, HID_FIELD_FEATURE);
    f->apc_manuf_date    = hid_find_field(m, UP_APC_MANUF_DATE, 1, HID_FIELD_FEATURE);
    f->apc_batt_cap_startup = hid_find_field(m, UP_APC_BATT_CAP_STARTUP, 1, HID_FIELD_FEATURE);

    /* Battery settings / info */
    f->remaining_time_limit = hid_find_field(m, UP_REMAINING_TIME_LIMIT, 1, HID_FIELD_FEATURE);
    f->remaining_cap_limit  = hid_find_field(m, UP_REMAINING_CAP_LIMIT, 1, HID_FIELD_FEATURE);
    f->warning_cap_limit    = hid_find_field(m, UP_WARNING_CAP_LIMIT, 1, HID_FIELD_FEATURE);
    f->design_capacity      = hid_find_field(m, UP_DESIGN_CAPACITY, 1, HID_FIELD_FEATURE);
    f->manufacture_date     = hid_find_field(m, UP_MANUFACTURE_DATE, 1, HID_FIELD_FEATURE);

    /* Log resolution results */
    struct { const char *name; const hid_field_t *field; } fields[] = {
        { "input_voltage",      f->input_voltage },
        { "output_voltage",     f->output_voltage },
        { "battery_voltage",    f->battery_voltage },
        { "remaining_capacity", f->remaining_capacity },
        { "runtime_to_empty",   f->runtime_to_empty },
        { "percent_load",       f->percent_load },
        { "output_frequency",   f->output_frequency },
        { "transfer_low",       f->transfer_low },
        { "transfer_high",      f->transfer_high },
        { "config_apparent_power", f->config_apparent_power },
        { "config_active_power",   f->config_active_power },
        { "test",               f->test },
        { "alarm_control",      f->alarm_control },
        { "delay_before_shutdown", f->delay_before_shutdown },
        { "delay_before_startup",  f->delay_before_startup },
        { "delay_before_reboot",   f->delay_before_reboot },
        { "module_reset",       f->module_reset },
        { "apc_sensitivity",    f->apc_sensitivity },
        { "apc_lights_test",    f->apc_lights_test },
        { "apc_batt_repl_date", f->apc_batt_repl_date },
        { "apc_manuf_date",     f->apc_manuf_date },
        { "apc_batt_cap_startup", f->apc_batt_cap_startup },
        { "remaining_time_limit", f->remaining_time_limit },
        { "remaining_cap_limit",  f->remaining_cap_limit },
        { "warning_cap_limit",    f->warning_cap_limit },
        { "design_capacity",    f->design_capacity },
        { "manufacture_date",   f->manufacture_date },
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (fields[i].field)
            log_info("backups_hid: %s -> RID 0x%02x", fields[i].name, fields[i].field->report_id);
    }
}

/* Forward declaration */
static void fixup_config_regs(hid_transport_t *t);

/* --- Connection lifecycle --- */

static void *backups_connect(const ups_conn_params_t *params)
{
    if (params->type != UPS_CONN_USB) return NULL;

    char devpath[PATH_MAX];
    char sysfs[PATH_MAX] = "";
    char hidraw_name[NAME_MAX] = "";

    if (find_hidraw_device(params->usb.vendor_id, params->usb.product_id,
                           devpath, sizeof(devpath),
                           sysfs, sizeof(sysfs),
                           hidraw_name) != 0) {
        log_warn("backups_hid: no hidraw device found for %04x:%04x",
                 params->usb.vendor_id, params->usb.product_id);
        return NULL;
    }

    int fd = open(devpath, O_RDWR);
    if (fd < 0) {
        log_warn("backups_hid: cannot open %s", devpath);
        return NULL;
    }

    hid_transport_t *t = calloc(1, sizeof(*t));
    if (!t) { close(fd); return NULL; }
    t->fd = fd;
    snprintf(t->sysfs_base, sizeof(t->sysfs_base), "%s", sysfs);
    snprintf(t->hidraw_name, sizeof(t->hidraw_name), "%s", hidraw_name);

    log_info("backups_hid: opened %s", devpath);

    if (parse_hid_descriptor(t) != 0) {
        log_warn("backups_hid: continuing without descriptor (reads may be wrong)");
    } else {
        resolve_fields(t);
        fixup_config_regs(t);
    }

    /* Detect topology: PowerConverter (0x84:0x16) present = line-interactive */
    t->topology = UPS_TOPO_STANDBY;
    for (size_t i = 0; i < t->map.count; i++) {
        const hid_field_t *fld = &t->map.fields[i];
        for (int j = 0; j < fld->path_depth; j++) {
            if (fld->usage_path[j] == HID_UP(HID_PAGE_POWER, HID_USAGE_POWER_CONVERTER)) {
                t->topology = UPS_TOPO_LINE_INTERACTIVE;
                log_info("backups_hid: PowerConverter collection found — line-interactive");
                goto topo_done;
            }
        }
    }
    log_info("backups_hid: no PowerConverter — standby topology");
topo_done:

    return t;
}

static ups_topology_t backups_get_topology(void *transport)
{
    hid_transport_t *t = transport;
    return t->topology;
}

static void backups_disconnect(void *transport)
{
    hid_transport_t *t = transport;
    if (t) {
        if (t->fd >= 0) close(t->fd);
        hid_report_map_free(&t->map);
        free(t);
    }
}

static int backups_detect(void *transport)
{
    hid_transport_t *t = transport;

    if (t->f.remaining_capacity) {
        int32_t cap = hid_field_read_raw(t->fd, t->f.remaining_capacity);
        if (cap == 0)
            cap = hid_field_read_raw(t->fd, t->f.remaining_capacity);
        (void)cap;
    }

    char product[128];
    read_sysfs_string(t->sysfs_base, "product", product, sizeof(product));
    if (strstr(product, "Back-UPS") != NULL)
        return 1;

    char mfg[128];
    read_sysfs_string(t->sysfs_base, "manufacturer", mfg, sizeof(mfg));
    return strstr(mfg, "American Power Conversion") != NULL;
}

/* --- Status reading ---
 *
 * The PresentStatus collection contains 1-bit fields packed into a
 * multi-byte report. We read the entire report and extract bits by
 * their usage from the parsed descriptor. */

static int read_status_bit(hid_transport_t *t, uint16_t page, uint16_t usage,
                           const uint8_t *report, int report_len)
{
    for (size_t i = 0; i < t->map.count; i++) {
        const hid_field_t *f = &t->map.fields[i];
        if (f->type != HID_FIELD_FEATURE) continue;
        if (f->bit_size != 1) continue;
        if (f->usage_page != page || f->usage_id != usage) continue;

        uint16_t byte_idx = (uint16_t)(1 + f->bit_offset / 8);
        uint16_t bit_idx  = (uint16_t)(f->bit_offset % 8);
        if (byte_idx < (uint16_t)report_len)
            return (report[byte_idx] >> bit_idx) & 1;
    }
    return 0;
}

static int backups_read_status(void *transport, ups_data_t *data)
{
    hid_transport_t *t = transport;

    /* Find status report ID from any 1-bit ACPresent field */
    uint8_t status_rid = 0;
    for (size_t i = 0; i < t->map.count; i++) {
        const hid_field_t *f = &t->map.fields[i];
        if (f->type == HID_FIELD_FEATURE && f->bit_size == 1 &&
            f->usage_page == HID_PAGE_BATTERY && f->usage_id == HID_USAGE_BAT_AC_PRESENT) {
            status_rid = f->report_id;
            break;
        }
    }

    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    buf[0] = status_rid;
    int rc = ioctl(t->fd, HIDIOCGFEATURE(sizeof(buf)), buf);
    if (rc < 0) return UPS_ERR_IO;

    /* Clear fields we'll populate */
    data->status = 0;
    data->sig_status = 0;
    data->general_error = 0;
    data->bat_system_error = 0;

    /* --- Standard HID PDC PresentStatus bits --- */

    /* ACPresent (85:D0) → ONLINE */
    if (read_status_bit(t, HID_PAGE_BATTERY, HID_USAGE_BAT_AC_PRESENT, buf, rc))
        data->status |= UPS_ST_ONLINE;

    /* Discharging (85:45) → ON_BATTERY */
    if (read_status_bit(t, HID_PAGE_BATTERY, HID_USAGE_BAT_DISCHARGING, buf, rc))
        data->status |= UPS_ST_ON_BATTERY;

    /* Overload (84:65) → OVERLOAD */
    if (read_status_bit(t, HID_PAGE_POWER, HID_USAGE_OVERLOAD, buf, rc))
        data->status |= UPS_ST_OVERLOAD;

    /* ShutdownRequested (84:68) → SHUT_PENDING */
    if (read_status_bit(t, HID_PAGE_POWER, HID_USAGE_SHUTDOWN_REQUESTED, buf, rc))
        data->status |= UPS_ST_SHUT_PENDING;

    /* ShutdownImminent (84:69) */
    if (read_status_bit(t, HID_PAGE_POWER, HID_USAGE_SHUTDOWN_IMMINENT, buf, rc))
        data->sig_status |= UPS_SIG_SHUTDOWN_IMMINENT;

    /* BelowRemainingCapacityLimit (85:42) → INPUT_BAD (signals low battery) */
    if (read_status_bit(t, HID_PAGE_BATTERY, HID_USAGE_BAT_BELOW_CAP_LIMIT, buf, rc))
        data->status |= UPS_ST_INPUT_BAD;

    /* RemainingTimeLimitExpired (85:43) */
    if (read_status_bit(t, HID_PAGE_BATTERY, HID_USAGE_BAT_REMAINING_TIME_EXP, buf, rc))
        data->sig_status |= UPS_SIG_SHUTDOWN_IMMINENT;

    /* CommunicationLost (84:73) */
    if (read_status_bit(t, HID_PAGE_POWER, HID_USAGE_COMMUNICATIONS_LOST, buf, rc))
        data->general_error |= UPS_GENERR_INTERNAL_COMM;

    /* NeedReplacement (85:4B) */
    if (read_status_bit(t, HID_PAGE_BATTERY, HID_USAGE_BAT_NEED_REPLACEMENT, buf, rc))
        data->bat_system_error |= UPS_BATERR_REPLACE;

    /* BatteryPresent (85:D1) → if NOT present, set DISCONNECTED */
    if (!read_status_bit(t, HID_PAGE_BATTERY, HID_USAGE_BAT_BATTERY_PRESENT, buf, rc))
        data->bat_system_error |= UPS_BATERR_DISCONNECTED;

    /* VoltageNotRegulated (85:DB) — informational for line-interactive */
    if (read_status_bit(t, HID_PAGE_BATTERY, HID_USAGE_BAT_VOLTAGE_NOT_REG, buf, rc))
        data->status |= UPS_ST_FAULT;

    /* Boost (84:6E) → AVR_BOOST */
    if (read_status_bit(t, HID_PAGE_POWER, HID_USAGE_BOOST, buf, rc))
        data->status |= UPS_ST_AVR_BOOST;

    /* Buck (84:6F) → AVR_TRIM */
    if (read_status_bit(t, HID_PAGE_POWER, HID_USAGE_BUCK, buf, rc))
        data->status |= UPS_ST_AVR_TRIM;

    /* --- Test status (84:58) --- */
    if (t->f.test) {
        int32_t test_val = hid_field_read_raw(t->fd, t->f.test);
        /* Write values: 0=none, 1=quick, 2=deep, 3=abort
         * Read values: 1=pass, 2=warning, 3=error, 4=aborted, 5=in progress, 6=none */
        if (test_val == 5)
            data->status |= UPS_ST_TEST;
        data->bat_test_status = (uint16_t)test_val;
    }

    /* Synthesize outlet state */
    if (data->status & UPS_ST_ONLINE)
        data->outlet_mog = (1u << 0); /* on */
    else if (data->status & UPS_ST_ON_BATTERY)
        data->outlet_mog = (1u << 0); /* still on, running from battery */
    else
        data->outlet_mog = (1u << 1); /* off */

    return 0;
}

/* --- Dynamic reads --- */

static int backups_read_dynamic(void *transport, ups_data_t *data)
{
    hid_transport_t *t = transport;

    data->charge_pct       = hid_field_read_scaled(t->fd, t->f.remaining_capacity);
    data->battery_voltage  = hid_field_read_scaled(t->fd, t->f.battery_voltage);
    data->runtime_sec      = (uint32_t)hid_field_read_scaled(t->fd, t->f.runtime_to_empty);
    data->input_voltage    = hid_field_read_scaled(t->fd, t->f.input_voltage);

    /* Output voltage: no dedicated sensor on Back-UPS HID.
     * On battery → nominal config voltage; online → passthrough from input. */
    if (data->status & UPS_ST_ON_BATTERY)
        data->output_voltage = hid_field_read_scaled(t->fd, t->f.output_voltage);
    else
        data->output_voltage = data->input_voltage;

    data->load_pct         = hid_field_read_scaled(t->fd, t->f.percent_load);
    data->output_frequency = hid_field_read_scaled(t->fd, t->f.output_frequency);

    /* Derive output current: I = (load% / 100) * nominal_watts / output_voltage */
    if (data->output_voltage > 0 && t->nominal_watts > 0)
        data->output_current = (data->load_pct / 100.0)
                             * (double)t->nominal_watts
                             / data->output_voltage;
    else
        data->output_current = 0;

    /* Timer fields from delay controls */
    if (t->f.delay_before_shutdown) {
        int32_t v = hid_field_read_raw(t->fd, t->f.delay_before_shutdown);
        data->timer_shutdown = (int16_t)v;
    }
    if (t->f.delay_before_startup) {
        int32_t v = hid_field_read_raw(t->fd, t->f.delay_before_startup);
        data->timer_start = (int16_t)v;
    }
    if (t->f.delay_before_reboot) {
        int32_t v = hid_field_read_raw(t->fd, t->f.delay_before_reboot);
        data->timer_reboot = (int32_t)v;
    }

    /* No bypass or efficiency on Back-UPS */
    data->bypass_voltage   = 0;
    data->bypass_frequency = 0;
    data->efficiency       = -1;

    return 0;
}

static int backups_read_inventory(void *transport, ups_inventory_t *inv)
{
    hid_transport_t *t = transport;
    memset(inv, 0, sizeof(*inv));

    read_sysfs_string(t->sysfs_base, "product", inv->model, sizeof(inv->model));
    read_sysfs_string(t->sysfs_base, "serial", inv->serial, sizeof(inv->serial));

    char *fw = strstr(inv->model, "FW:");
    if (fw)
        snprintf(inv->firmware, sizeof(inv->firmware), "%s", fw);

    /* Try descriptor first, then fall back to known model table */
    inv->nominal_va = 0;
    inv->nominal_watts = 0;

    if (t->f.config_apparent_power) {
        int32_t va = hid_field_read_raw(t->fd, t->f.config_apparent_power);
        if (va > 0)
            inv->nominal_va = (uint16_t)va;
    }
    if (t->f.config_active_power) {
        int32_t w = hid_field_read_raw(t->fd, t->f.config_active_power);
        if (w > 0)
            inv->nominal_watts = (uint16_t)w;
    }

    if (inv->nominal_va > 0)
        log_info("backups_hid: VA from descriptor — %u VA", inv->nominal_va);
    if (inv->nominal_watts > 0)
        log_info("backups_hid: watts from descriptor — %u W", inv->nominal_watts);

    if (inv->nominal_va == 0 || inv->nominal_watts == 0) {
        for (size_t i = 0; i < sizeof(known_models) / sizeof(known_models[0]); i++) {
            if (strstr(inv->model, known_models[i].model)) {
                if (inv->nominal_va == 0) {
                    inv->nominal_va = known_models[i].va;
                    log_info("backups_hid: VA from model lookup '%s' — %u VA",
                             known_models[i].model, inv->nominal_va);
                }
                if (inv->nominal_watts == 0) {
                    inv->nominal_watts = known_models[i].watts;
                    log_info("backups_hid: watts from model lookup '%s' — %u W",
                             known_models[i].model, inv->nominal_watts);
                }
                break;
            }
        }
    }

    if (inv->nominal_va == 0)
        log_warn("backups_hid: could not determine VA rating");
    if (inv->nominal_watts == 0)
        log_warn("backups_hid: could not determine watt rating");

    inv->sog_config = 0;
    t->nominal_watts = inv->nominal_watts;

    return 0;
}

static int backups_read_thresholds(void *transport,
                                    uint16_t *transfer_high,
                                    uint16_t *transfer_low)
{
    hid_transport_t *t = transport;

    *transfer_high = (uint16_t)hid_field_read_scaled(t->fd, t->f.transfer_high);
    *transfer_low  = (uint16_t)hid_field_read_scaled(t->fd, t->f.transfer_low);
    return 0;
}

/* --- Commands --- */

static int backups_cmd_shutdown(void *transport)
{
    hid_transport_t *t = transport;
    if (!t->f.delay_before_shutdown) return -1;

    /* Per APC AN178: write DelayBeforeStartup first so the UPS
     * automatically restores output when AC returns, then write
     * DelayBeforeShutdown to begin the shutdown countdown.
     * Without the startup write, the UPS stays off permanently. */
    if (t->f.delay_before_startup) {
        /* 0 = start immediately when AC is restored */
        uint8_t sbuf[3] = { t->f.delay_before_startup->report_id, 0, 0 };
        hid_set_feature(t->fd, sbuf[0], sbuf, sizeof(sbuf));
    }

    uint8_t buf[3] = { t->f.delay_before_shutdown->report_id, 60, 0 };
    return hid_set_feature(t->fd, buf[0], buf, sizeof(buf)) >= 0 ? 0 : -1;
}

static int backups_cmd_abort_shutdown(void *transport)
{
    hid_transport_t *t = transport;
    if (!t->f.delay_before_shutdown) return -1;
    /* Write -1 (0xFFFF) to abort any pending shutdown */
    uint8_t buf[3] = { t->f.delay_before_shutdown->report_id, 0xFF, 0xFF };
    return hid_set_feature(t->fd, buf[0], buf, sizeof(buf)) >= 0 ? 0 : -1;
}

static int backups_cmd_battery_test(void *transport)
{
    hid_transport_t *t = transport;
    if (!t->f.test) return -1;
    /* Test: 1 = Quick test (§4.1.4) */
    uint8_t buf[2] = { t->f.test->report_id, 0x01 };
    return hid_set_feature(t->fd, buf[0], buf, sizeof(buf)) >= 0 ? 0 : -1;
}

/* Note: Test=2 (deep test / runtime calibration) and Test=3 (abort) are
 * intentionally not exposed. Consumer Back-UPS firmware (BR/BN/BX) advertises
 * the field but rejects deep test — it transitions immediately to "aborted"
 * (state 4) without entering "in progress" (state 5). Calibration on these
 * units is a front-panel LCD operation only (Test & Diags → Battery
 * Calibration). Confirmed against BR1500MS2 FW:969.e2 and corroborated by
 * NUT/apcupsd community reports and Schneider's own PowerChute docs. */

static int backups_cmd_mute_alarm(void *transport)
{
    hid_transport_t *t = transport;
    if (!t->f.alarm_control) return -1;
    /* AudibleAlarmControl: 3 = Muted (§4.1.4) */
    uint8_t buf[2] = { t->f.alarm_control->report_id, 0x03 };
    return hid_set_feature(t->fd, buf[0], buf, sizeof(buf)) >= 0 ? 0 : -1;
}

static int backups_cmd_cancel_mute(void *transport)
{
    hid_transport_t *t = transport;
    if (!t->f.alarm_control) return -1;
    /* AudibleAlarmControl: 2 = Enabled (§4.1.4) */
    uint8_t buf[2] = { t->f.alarm_control->report_id, 0x02 };
    return hid_set_feature(t->fd, buf[0], buf, sizeof(buf)) >= 0 ? 0 : -1;
}

static int backups_cmd_beep_test(void *transport)
{
    hid_transport_t *t = transport;
    if (!t->f.apc_lights_test) return -1;
    /* APCLightsAndBeeperTest (FF86:72): write 1 to execute */
    uint8_t buf[2] = { t->f.apc_lights_test->report_id, 0x01 };
    return hid_set_feature(t->fd, buf[0], buf, sizeof(buf)) >= 0 ? 0 : -1;
}

static int backups_cmd_clear_faults(void *transport)
{
    hid_transport_t *t = transport;
    if (!t->f.module_reset) return -1;
    /* ModuleReset: 2 = Reset Module's Alarms (§4.1.4) */
    uint8_t buf[2] = { t->f.module_reset->report_id, 0x02 };
    return hid_set_feature(t->fd, buf[0], buf, sizeof(buf)) >= 0 ? 0 : -1;
}

/* --- Config registers ---
 *
 * The reg_addr field stores the HID report ID resolved at connect time.
 * config_read and config_write use the cached field pointer for reads
 * and direct HID set_feature for writes. */

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

/* Config registers — populated with resolved report IDs at connect time.
 * Initial reg_addr values are placeholders (0). */
static ups_config_reg_t backups_config_regs[] = {
    /* --- Group: transfer --- */
    { "transfer_low", "Low Transfer Voltage", "V", "transfer",
      0, 1, UPS_CFG_SCALAR, 1, 1,
      .meta.scalar = { 0, 0 } },
    { "transfer_high", "High Transfer Voltage", "V", "transfer",
      0, 1, UPS_CFG_SCALAR, 1, 1,
      .meta.scalar = { 0, 0 } },

    /* --- Group: power_quality --- */
    { "sensitivity", "Input Sensitivity", NULL, "power_quality",
      0, 1, UPS_CFG_BITFIELD, 1, 1,
      .meta.bitfield = { sensitivity_opts,
                         sizeof(sensitivity_opts) / sizeof(sensitivity_opts[0]) } },

    /* --- Group: alarm --- */
    { "alarm_setting", "Audible Alarm", NULL, "alarm",
      0, 1, UPS_CFG_BITFIELD, 1, 1,
      .meta.bitfield = { alarm_opts,
                         sizeof(alarm_opts) / sizeof(alarm_opts[0]) } },

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
      .meta.scalar = { 0, 0 } },

    /* --- Group: info --- */
    { "nominal_voltage", "Nominal Input Voltage", "V", "info",
      0, 1, UPS_CFG_SCALAR, 1, 0,
      .meta.scalar = { 0, 0 } },
    { "nominal_va", "Nominal Apparent Power", "VA", "info",
      0, 1, UPS_CFG_SCALAR, 1, 0,
      .meta.scalar = { 0, 0 } },
    { "design_capacity", "Design Capacity", "%", "info",
      0, 1, UPS_CFG_SCALAR, 1, 0,
      .meta.scalar = { 0, 0 } },
};

#define BACKUPS_CFG_COUNT (sizeof(backups_config_regs) / sizeof(backups_config_regs[0]))

/* Config register index constants (must match array order above) */
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
};

/* Populate config register metadata from resolved HID fields */
static void fixup_one_reg(ups_config_reg_t *reg, const hid_field_t *field)
{
    if (!field) return;
    reg->reg_addr = field->report_id;
    if (reg->type == UPS_CFG_SCALAR) {
        reg->meta.scalar.min = field->logical_min;
        reg->meta.scalar.max = field->logical_max;
    }
}

static void fixup_config_regs(hid_transport_t *t)
{
    fixup_one_reg(&backups_config_regs[CFG_TRANSFER_LOW],       t->f.transfer_low);
    fixup_one_reg(&backups_config_regs[CFG_TRANSFER_HIGH],      t->f.transfer_high);
    fixup_one_reg(&backups_config_regs[CFG_SENSITIVITY],        t->f.apc_sensitivity);
    fixup_one_reg(&backups_config_regs[CFG_ALARM_SETTING],      t->f.alarm_control);
    fixup_one_reg(&backups_config_regs[CFG_LOW_BATTERY_WARNING], t->f.remaining_time_limit);
    fixup_one_reg(&backups_config_regs[CFG_LOW_BATTERY_THRESHOLD], t->f.remaining_cap_limit);
    fixup_one_reg(&backups_config_regs[CFG_WARNING_CAPACITY],   t->f.warning_cap_limit);
    fixup_one_reg(&backups_config_regs[CFG_MANUFACTURE_DATE],   t->f.manufacture_date);
    fixup_one_reg(&backups_config_regs[CFG_NOMINAL_VOLTAGE],    t->f.output_voltage);
    fixup_one_reg(&backups_config_regs[CFG_NOMINAL_VA],         t->f.config_apparent_power);
    fixup_one_reg(&backups_config_regs[CFG_DESIGN_CAPACITY],    t->f.design_capacity);
}

/* Convert USB battery ManufactureDate (85:85) to days since 2000-01-01.
 * USB encoding: [(year-1980)*512 + month*32 + day]
 * The API's date formatter uses "days since 2000-01-01" to render ISO dates,
 * so we convert here to match the SRT driver's convention. */
static uint16_t usb_date_to_days_since_2000(uint16_t raw)
{
    if (raw == 0) return 0;
    int day   = raw & 0x1F;
    int month = (raw >> 5) & 0x0F;
    int year  = ((raw >> 9) & 0x7F) + 1980;

    /* Convert to epoch, then to days since 2000-01-01 */
    struct tm tm = { .tm_year = year - 1900, .tm_mon = month - 1,
                     .tm_mday = day, .tm_isdst = -1 };
    time_t t = mktime(&tm);
    time_t epoch_2000 = 946684800; /* 2000-01-01 00:00:00 UTC */
    if (t < epoch_2000) return 0;
    return (uint16_t)((t - epoch_2000) / 86400);
}

/* Find the resolved HID field for a config register */
static const hid_field_t *find_config_field(const hid_transport_t *t,
                                             const ups_config_reg_t *reg)
{
    for (size_t i = 0; i < t->map.count; i++) {
        if (t->map.fields[i].report_id == reg->reg_addr &&
            t->map.fields[i].type == HID_FIELD_FEATURE)
            return &t->map.fields[i];
    }
    return NULL;
}

static int backups_config_read(void *transport, const ups_config_reg_t *reg,
                                uint16_t *raw_value, char *str_buf, size_t str_bufsz)
{
    (void)str_buf; (void)str_bufsz;
    hid_transport_t *t = transport;

    const hid_field_t *field = find_config_field(t, reg);
    if (!field) return UPS_ERR_NOT_SUPPORTED;

    int32_t val = hid_field_read_raw(t->fd, field);

    /* For bitfield registers with multi-byte HID fields (e.g., APC
     * sensitivity is 24-bit but only the low byte is the enum value),
     * mask to the byte that contains the setting. */
    if (reg->type == UPS_CFG_BITFIELD && field->bit_size > 8)
        val &= 0xFF;

    /* Convert USB battery date encoding to days-since-2000 so the
     * API's standard date formatter handles it identically to SRT. */
    if (strcmp(reg->name, "manufacture_date") == 0)
        val = (int32_t)usb_date_to_days_since_2000((uint16_t)val);

    if (raw_value) *raw_value = (uint16_t)val;

    return UPS_OK;
}

static int backups_config_write(void *transport, const ups_config_reg_t *reg, uint16_t value)
{
    hid_transport_t *t = transport;

    if (!reg->writable)
        return UPS_ERR_NOT_SUPPORTED;

    const hid_field_t *field = find_config_field(t, reg);
    if (!field) return UPS_ERR_NOT_SUPPORTED;

    /* Build write buffer: [report_id, data bytes...]
     * Size = 1 (report ID) + ceil(bit_size / 8) */
    uint16_t data_bytes = (uint16_t)((field->bit_size + 7u) / 8u);
    uint8_t buf[8] = { 0 };
    buf[0] = field->report_id;

    if (reg->type == UPS_CFG_BITFIELD && data_bytes > 1) {
        /* Multi-byte bitfield: read current report, modify only
         * the value byte, preserve the rest */
        uint8_t cur[8] = { 0 };
        cur[0] = field->report_id;
        uint16_t rlen = (uint16_t)(1u + data_bytes);
        if (ioctl(t->fd, HIDIOCGFEATURE(rlen), cur) < 0)
            return UPS_ERR_IO;
        memcpy(buf, cur, rlen);
        buf[1] = (uint8_t)value;  /* enum value goes in low byte */
    } else {
        buf[1] = (uint8_t)(value & 0xFF);
        if (data_bytes > 1)
            buf[2] = (uint8_t)((value >> 8) & 0xFF);
    }

    uint16_t wlen = (uint16_t)(1u + data_bytes);
    if (hid_set_feature(t->fd, buf[0], buf, wlen) < 0)
        return UPS_ERR_IO;

    /* Read back and verify */
    uint16_t readback;
    if (backups_config_read(transport, reg, &readback, NULL, 0) != UPS_OK)
        return UPS_ERR_IO;

    return (readback == value) ? UPS_OK : UPS_ERR_IO;
}

/* --- Command descriptors --- */

static const ups_cmd_desc_t backups_commands[] = {
    { "shutdown", "Shutdown UPS", "Send the UPS shutdown command (60 second delay)",
      "power", "Shutdown UPS?",
      "This sends the shutdown command to the UPS with a 60 second delay.",
      UPS_CMD_SIMPLE, UPS_CMD_DANGER, UPS_CMD_IS_SHUTDOWN, 0,
      backups_cmd_shutdown, NULL },

    { "abort_shutdown", "Abort Shutdown", "Cancel a pending shutdown countdown",
      "power", "Abort Shutdown?",
      "This will cancel any pending shutdown countdown.",
      UPS_CMD_SIMPLE, UPS_CMD_DEFAULT, 0, 0,
      backups_cmd_abort_shutdown, NULL },

    { "battery_test", "Battery Test", "Run a quick battery self-test",
      "diagnostics", "Start Battery Test?",
      "This will run a brief self-test to verify battery health.",
      UPS_CMD_SIMPLE, UPS_CMD_DEFAULT, 0, 0,
      backups_cmd_battery_test, NULL },

    { "clear_faults", "Clear Faults", "Reset latched fault flags",
      "diagnostics", "Clear Faults?",
      "This will reset any latched alarm or fault indicators.",
      UPS_CMD_SIMPLE, UPS_CMD_DEFAULT, 0, 0,
      backups_cmd_clear_faults, NULL },

    { "beep_short", "Beep / LED Test", "Test the audible alarm and panel LEDs",
      "alarm", "Run Beep Test?",
      "This will briefly activate the audible alarm and panel LEDs.",
      UPS_CMD_SIMPLE, UPS_CMD_DEFAULT, 0, 0,
      backups_cmd_beep_test, NULL },

    { "mute", "Mute Alarm", "Silence the UPS audible alarm",
      "alarm", "Mute Alarm?",
      "This will silence the UPS audible alarm.",
      UPS_CMD_SIMPLE, UPS_CMD_DEFAULT, UPS_CMD_IS_MUTE, 0,
      backups_cmd_mute_alarm, NULL },

    { "unmute", "Unmute Alarm", "Re-enable the UPS audible alarm",
      "alarm", "Unmute Alarm?",
      "This will re-enable the UPS audible alarm.",
      UPS_CMD_SIMPLE, UPS_CMD_DEFAULT, 0, 0,
      backups_cmd_cancel_mute, NULL },
};

/* --- Driver definition --- */

const ups_driver_t ups_driver_backups_hid = {
    .name                = "backups_hid",
    .conn_type           = UPS_CONN_USB,
    .topology            = UPS_TOPO_STANDBY,  /* default; overridden by get_topology */
    .connect             = backups_connect,
    .disconnect          = backups_disconnect,
    .detect              = backups_detect,
    .get_topology        = backups_get_topology,
    /* Advertise all capabilities; individual handlers return -1 if
     * the underlying HID field is not present on this device. */
    .caps                = UPS_CAP_SHUTDOWN | UPS_CAP_BATTERY_TEST |
                           UPS_CAP_CLEAR_FAULTS |
                           UPS_CAP_MUTE | UPS_CAP_BEEP,
    .freq_settings       = NULL,
    .freq_settings_count = 0,
    .config_regs         = backups_config_regs,
    .config_regs_count   = BACKUPS_CFG_COUNT,
    .read_status         = backups_read_status,
    .read_dynamic        = backups_read_dynamic,
    .read_inventory      = backups_read_inventory,
    .read_thresholds     = backups_read_thresholds,
    .commands            = backups_commands,
    .commands_count      = sizeof(backups_commands) / sizeof(backups_commands[0]),
    .config_read         = backups_config_read,
    .config_write        = backups_config_write,
};
