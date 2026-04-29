/* realpath() requires _XOPEN_SOURCE >= 500 on some toolchains */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 500
#endif

#include "hid_pdc_core.h"
#include "ups.h"
#include "ups_format.h"

#include <cutils/log.h>
#include <cutils/mem.h>

#include <dirent.h>
#include <fcntl.h>
#include <linux/hidraw.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* --- Standard PDC usage paths --------------------------------------------
 *
 * Collection-qualified paths matched as suffixes against the parsed
 * descriptor's full collection path. Vendor adapters are free to declare
 * additional paths in their own files for vendor-page extensions. */

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

/* Power controls (typically not nested under a sub-collection) */
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

/* Nameplate ratings */
static const uint32_t UP_CONFIG_APPARENT_POWER[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_CONFIG_APPARENT_POWER) };

static const uint32_t UP_CONFIG_ACTIVE_POWER[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_CONFIG_ACTIVE_POWER) };

/* Battery settings / info */
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

/* --- PowerSummary-collection fallbacks ---
 *
 * Some vendors (notably CyberPower PowerPanel HID, but also a few Eaton
 * and Tripp-Lite SKUs per NUT) put battery telemetry under a
 * Power.PowerSummary collection rather than Power.Battery. The HID PDC
 * spec allows either; resolve_standard_fields tries the Battery path
 * first, then falls back to PowerSummary so we don't have to special-case
 * each vendor. */
static const uint32_t UP_PSUM_BATTERY_VOLTAGE[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_POWER_SUMMARY),
    HID_UP(HID_PAGE_POWER, HID_USAGE_VOLTAGE) };

static const uint32_t UP_PSUM_REMAINING_CAPACITY[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_POWER_SUMMARY),
    HID_UP(HID_PAGE_BATTERY, HID_USAGE_BAT_REMAINING_CAPACITY) };

static const uint32_t UP_PSUM_RUNTIME_TO_EMPTY[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_POWER_SUMMARY),
    HID_UP(HID_PAGE_BATTERY, HID_USAGE_BAT_RUNTIME_TO_EMPTY) };

static const uint32_t UP_PSUM_REMAINING_TIME_LIMIT[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_POWER_SUMMARY),
    HID_UP(HID_PAGE_BATTERY, HID_USAGE_BAT_REMAINING_TIME_LIM) };

static const uint32_t UP_PSUM_REMAINING_CAP_LIMIT[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_POWER_SUMMARY),
    HID_UP(HID_PAGE_BATTERY, HID_USAGE_BAT_REMAINING_CAP_LIM) };

static const uint32_t UP_PSUM_WARNING_CAP_LIMIT[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_POWER_SUMMARY),
    HID_UP(HID_PAGE_BATTERY, HID_USAGE_BAT_WARNING_CAP_LIMIT) };

static const uint32_t UP_PSUM_DESIGN_CAPACITY[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_POWER_SUMMARY),
    HID_UP(HID_PAGE_BATTERY, HID_USAGE_BAT_DESIGN_CAPACITY) };

static const uint32_t UP_PSUM_MANUFACTURE_DATE[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_POWER_SUMMARY),
    HID_UP(HID_PAGE_BATTERY, HID_USAGE_BAT_MANUFACTURE_DATE) };

/* --- hidraw discovery ---------------------------------------------------- */

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

        CUTILS_AUTOCLOSE FILE *f = fopen(uevent, "r");
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

        if (match) {  /* NOLINT(clang-analyzer-unix.Stream) */
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

/* --- Public helpers ------------------------------------------------------ */

int hid_pdc_set_feature(int fd, uint8_t report_id, const void *buf, size_t len)
{
    (void)report_id;
    return ioctl(fd, HIDIOCSFEATURE(len), buf);
}

void hid_pdc_read_sysfs_string(const char *base, const char *attr,
                                char *buf, size_t bufsz)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", base, attr);
    CUTILS_AUTOCLOSE FILE *f = fopen(path, "r");
    if (!f) { buf[0] = '\0'; return; }
    if (!fgets(buf, (int)bufsz, f))
        buf[0] = '\0';  /* NOLINT(clang-analyzer-unix.Stream) */
    size_t len = strlen(buf);
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
        buf[--len] = '\0';
}

const hid_field_t *hid_pdc_field_by_index(const hid_report_map_t *map,
                                           uint16_t index)
{
    if ((size_t)index >= map->count) return NULL;
    return &map->fields[index];
}

void hid_pdc_fixup_config_reg(ups_config_reg_t *reg,
                               const hid_field_t *field,
                               const hid_report_map_t *map)
{
    if (!field) return;
    /* The field's index in map->fields[] is uniquely identifying — unlike
     * report_id which can be shared by multiple fields packed into one
     * report (CP1500PFCLCD has three battery-capacity fields at RID 0x07,
     * for instance). */
    reg->reg_addr = (uint16_t)(field - &map->fields[0]);
    if (reg->type == UPS_CFG_SCALAR) {
        reg->meta.scalar.min = field->logical_min;
        reg->meta.scalar.max = field->logical_max;
    }
}

uint16_t hid_pdc_usb_date_to_days_since_2000(uint16_t raw)
{
    if (raw == 0) return 0;
    int day   = raw & 0x1F;
    int month = (raw >> 5) & 0x0F;
    int year  = ((raw >> 9) & 0x7F) + 1980;

    struct tm tm = { .tm_year = year - 1900, .tm_mon = month - 1,
                     .tm_mday = day, .tm_isdst = -1 };
    time_t t = mktime(&tm);
    time_t epoch_2000 = 946684800; /* 2000-01-01 00:00:00 UTC */
    if (t < epoch_2000) return 0;
    return (uint16_t)((t - epoch_2000) / 86400);
}

/* --- Descriptor parse + standard field resolution ------------------------ */

static int parse_hid_descriptor(hid_pdc_transport_t *t)
{
    char desc_path[PATH_MAX];
    snprintf(desc_path, sizeof(desc_path),
             "/sys/class/hidraw/%s/device/report_descriptor", t->hidraw_name);

    int fd = open(desc_path, O_RDONLY);
    if (fd < 0) {
        log_warn("hid_pdc: cannot read descriptor from %s", desc_path);
        return -1;
    }

    uint8_t desc[4096];
    ssize_t dlen = read(fd, desc, sizeof(desc));
    close(fd);

    if (dlen <= 0) {
        log_warn("hid_pdc: empty descriptor");
        return -1;
    }

    if (hid_parse_descriptor(desc, (size_t)dlen, &t->map) != 0) {
        log_warn("hid_pdc: descriptor parse failed");
        return -1;
    }

    log_info("hid_pdc: parsed %zu fields from HID descriptor", t->map.count);
    return 0;
}

static void resolve_standard_fields(hid_pdc_transport_t *t)
{
    hid_pdc_fields_t *p = &t->pdc;
    const hid_report_map_t *m = &t->map;

    p->input_voltage         = hid_find_field(m, UP_INPUT_VOLTAGE,         2, HID_FIELD_FEATURE);
    p->output_voltage        = hid_find_field(m, UP_INPUT_CONFIG_VOLTAGE,  2, HID_FIELD_FEATURE);
    p->battery_voltage       = hid_find_field(m, UP_BATTERY_VOLTAGE,       2, HID_FIELD_FEATURE);
    p->remaining_capacity    = hid_find_field(m, UP_REMAINING_CAPACITY,    2, HID_FIELD_FEATURE);
    p->runtime_to_empty      = hid_find_field(m, UP_RUNTIME_TO_EMPTY,      2, HID_FIELD_FEATURE);
    p->percent_load          = hid_find_field(m, UP_PERCENT_LOAD,          1, HID_FIELD_FEATURE);
    p->output_frequency      = hid_find_field(m, UP_OUTPUT_FREQUENCY,      2, HID_FIELD_FEATURE);

    p->transfer_low          = hid_find_field(m, UP_TRANSFER_LOW,          2, HID_FIELD_FEATURE);
    p->transfer_high         = hid_find_field(m, UP_TRANSFER_HIGH,         2, HID_FIELD_FEATURE);
    p->config_apparent_power = hid_find_field(m, UP_CONFIG_APPARENT_POWER, 1, HID_FIELD_FEATURE);
    p->config_active_power   = hid_find_field(m, UP_CONFIG_ACTIVE_POWER,   1, HID_FIELD_FEATURE);

    p->test                  = hid_find_field(m, UP_TEST,                  1, HID_FIELD_FEATURE);
    p->alarm_control         = hid_find_field(m, UP_ALARM_CONTROL,         1, HID_FIELD_FEATURE);
    p->delay_before_shutdown = hid_find_field(m, UP_DELAY_SHUTDOWN,        1, HID_FIELD_FEATURE);
    p->delay_before_startup  = hid_find_field(m, UP_DELAY_STARTUP,         1, HID_FIELD_FEATURE);
    p->delay_before_reboot   = hid_find_field(m, UP_DELAY_REBOOT,          1, HID_FIELD_FEATURE);
    p->module_reset          = hid_find_field(m, UP_MODULE_RESET,          1, HID_FIELD_FEATURE);

    p->remaining_time_limit  = hid_find_field(m, UP_REMAINING_TIME_LIMIT,  1, HID_FIELD_FEATURE);
    p->remaining_cap_limit   = hid_find_field(m, UP_REMAINING_CAP_LIMIT,   1, HID_FIELD_FEATURE);
    p->warning_cap_limit     = hid_find_field(m, UP_WARNING_CAP_LIMIT,     1, HID_FIELD_FEATURE);
    p->design_capacity       = hid_find_field(m, UP_DESIGN_CAPACITY,       1, HID_FIELD_FEATURE);
    p->manufacture_date      = hid_find_field(m, UP_MANUFACTURE_DATE,      1, HID_FIELD_FEATURE);

    /* PowerSummary-collection fallbacks. CyberPower CP1500PFCLCD (and
     * other PowerPanel HID devices) put the entire battery telemetry
     * subtree under Power.PowerSummary instead of Power.Battery, so the
     * primary lookups above all miss. Re-try each affected field via the
     * PowerSummary path; APC and other vendors that already resolved via
     * the Battery path skip the fallback. */
    if (!p->battery_voltage)
        p->battery_voltage = hid_find_field(m, UP_PSUM_BATTERY_VOLTAGE, 2, HID_FIELD_FEATURE);
    if (!p->remaining_capacity)
        p->remaining_capacity = hid_find_field(m, UP_PSUM_REMAINING_CAPACITY, 2, HID_FIELD_FEATURE);
    if (!p->runtime_to_empty)
        p->runtime_to_empty = hid_find_field(m, UP_PSUM_RUNTIME_TO_EMPTY, 2, HID_FIELD_FEATURE);
    if (!p->remaining_time_limit)
        p->remaining_time_limit = hid_find_field(m, UP_PSUM_REMAINING_TIME_LIMIT, 2, HID_FIELD_FEATURE);
    if (!p->remaining_cap_limit)
        p->remaining_cap_limit = hid_find_field(m, UP_PSUM_REMAINING_CAP_LIMIT, 2, HID_FIELD_FEATURE);
    if (!p->warning_cap_limit)
        p->warning_cap_limit = hid_find_field(m, UP_PSUM_WARNING_CAP_LIMIT, 2, HID_FIELD_FEATURE);
    if (!p->design_capacity)
        p->design_capacity = hid_find_field(m, UP_PSUM_DESIGN_CAPACITY, 2, HID_FIELD_FEATURE);
    if (!p->manufacture_date)
        p->manufacture_date = hid_find_field(m, UP_PSUM_MANUFACTURE_DATE, 2, HID_FIELD_FEATURE);

    /* Per-field resolution log so the friend-build first-run output makes
     * it obvious which standard PDC fields the device exposed. Vendor
     * adapters log their own vendor fields the same way. */
    struct { const char *name; const hid_field_t *field; } fields[] = {
        { "input_voltage",         p->input_voltage },
        { "output_voltage",        p->output_voltage },
        { "battery_voltage",       p->battery_voltage },
        { "remaining_capacity",    p->remaining_capacity },
        { "runtime_to_empty",      p->runtime_to_empty },
        { "percent_load",          p->percent_load },
        { "output_frequency",      p->output_frequency },
        { "transfer_low",          p->transfer_low },
        { "transfer_high",         p->transfer_high },
        { "config_apparent_power", p->config_apparent_power },
        { "config_active_power",   p->config_active_power },
        { "test",                  p->test },
        { "alarm_control",         p->alarm_control },
        { "delay_before_shutdown", p->delay_before_shutdown },
        { "delay_before_startup",  p->delay_before_startup },
        { "delay_before_reboot",   p->delay_before_reboot },
        { "module_reset",          p->module_reset },
        { "remaining_time_limit",  p->remaining_time_limit },
        { "remaining_cap_limit",   p->remaining_cap_limit },
        { "warning_cap_limit",     p->warning_cap_limit },
        { "design_capacity",       p->design_capacity },
        { "manufacture_date",      p->manufacture_date },
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        if (fields[i].field)
            log_info("hid_pdc: %s -> RID 0x%02x", fields[i].name,
                     fields[i].field->report_id);
        else
            log_info("hid_pdc: %s -> (not present)", fields[i].name);
    }
}

static void detect_topology(hid_pdc_transport_t *t)
{
    /* PowerConverter (0x84:0x16) collection present = line-interactive,
     * absent = standby. */
    t->topology = UPS_TOPO_STANDBY;
    for (size_t i = 0; i < t->map.count; i++) {
        const hid_field_t *fld = &t->map.fields[i];
        for (int j = 0; j < fld->path_depth; j++) {
            if (fld->usage_path[j] == HID_UP(HID_PAGE_POWER, HID_USAGE_POWER_CONVERTER)) {
                t->topology = UPS_TOPO_LINE_INTERACTIVE;
                log_info("hid_pdc: PowerConverter collection found — line-interactive");
                return;
            }
        }
    }
    log_info("hid_pdc: no PowerConverter collection — standby topology");
}

/* --- Lifecycle ----------------------------------------------------------- */

hid_pdc_transport_t *hid_pdc_open(const ups_conn_params_t *params)
{
    if (!params || params->type != UPS_CONN_USB) return NULL;

    char devpath[PATH_MAX];
    char sysfs[PATH_MAX] = "";
    char hidraw_name[NAME_MAX] = "";

    if (find_hidraw_device(params->usb.vendor_id, params->usb.product_id,
                           devpath, sizeof(devpath),
                           sysfs, sizeof(sysfs),
                           hidraw_name) != 0) {
        log_warn("hid_pdc: no hidraw device found for %04x:%04x",
                 params->usb.vendor_id, params->usb.product_id);
        return NULL;
    }

    int fd = open(devpath, O_RDWR);
    if (fd < 0) {
        log_warn("hid_pdc: cannot open %s", devpath);
        return NULL;
    }

    hid_pdc_transport_t *t = calloc(1, sizeof(*t));
    if (!t) { close(fd); return NULL; }
    t->fd = fd;
    snprintf(t->sysfs_base, sizeof(t->sysfs_base), "%s", sysfs);
    snprintf(t->hidraw_name, sizeof(t->hidraw_name), "%s", hidraw_name);

    log_info("hid_pdc: opened %s (vid=%04x pid=%04x)",
             devpath, params->usb.vendor_id, params->usb.product_id);

    if (parse_hid_descriptor(t) != 0) {
        log_warn("hid_pdc: continuing without descriptor (reads may be wrong)");
    } else {
        resolve_standard_fields(t);
    }

    detect_topology(t);
    return t;
}

void hid_pdc_close(hid_pdc_transport_t *t)
{
    if (!t) return;
    if (t->fd >= 0) close(t->fd);
    hid_report_map_free(&t->map);
    /* Vendor must free t->vendor before calling us. */
    free(t);
}

/* --- Standard PresentStatus reader -------------------------------------- */

static int read_status_bit(const hid_pdc_transport_t *t,
                           uint16_t page, uint16_t usage,
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

/* Probe whether a 1-bit FEATURE field with the given (page, usage) exists
 * in the descriptor at all — independent of its current value. Used to
 * distinguish "field reads as zero" from "field is missing entirely",
 * which matters for status bits whose absence shouldn't imply false
 * (notably BatteryPresent: missing means "we have no information", not
 * "battery is disconnected"). */
static int has_status_bit(const hid_pdc_transport_t *t,
                          uint16_t page, uint16_t usage)
{
    for (size_t i = 0; i < t->map.count; i++) {
        const hid_field_t *f = &t->map.fields[i];
        if (f->type == HID_FIELD_FEATURE && f->bit_size == 1 &&
            f->usage_page == page && f->usage_id == usage)
            return 1;
    }
    return 0;
}

int hid_pdc_read_status_standard(hid_pdc_transport_t *t, ups_data_t *data)
{
    /* Find the PresentStatus report ID by locating any 1-bit ACPresent
     * field. All PresentStatus bits live in the same report. */
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

    data->status = 0;
    data->sig_status = 0;
    data->general_error = 0;
    data->bat_system_error = 0;

    /* HID PDC has no equivalent of APC's UPSStatusChangeCause register;
     * mark the field unknown so event consumers don't append a misleading
     * "(reason: SystemInitialization)" suffix. */
    data->transfer_reason = UPS_TRANSFER_REASON_UNKNOWN;

    /* --- Standard PresentStatus bits ---------------------------------- */

    if (read_status_bit(t, HID_PAGE_BATTERY, HID_USAGE_BAT_AC_PRESENT, buf, rc))
        data->status |= UPS_ST_ONLINE;

    if (read_status_bit(t, HID_PAGE_BATTERY, HID_USAGE_BAT_DISCHARGING, buf, rc))
        data->status |= UPS_ST_ON_BATTERY;

    if (read_status_bit(t, HID_PAGE_POWER, HID_USAGE_OVERLOAD, buf, rc))
        data->status |= UPS_ST_OVERLOAD;

    if (read_status_bit(t, HID_PAGE_POWER, HID_USAGE_SHUTDOWN_REQUESTED, buf, rc))
        data->status |= UPS_ST_SHUT_PENDING;

    if (read_status_bit(t, HID_PAGE_POWER, HID_USAGE_SHUTDOWN_IMMINENT, buf, rc))
        data->sig_status |= UPS_SIG_SHUTDOWN_IMMINENT;

    if (read_status_bit(t, HID_PAGE_BATTERY, HID_USAGE_BAT_BELOW_CAP_LIMIT, buf, rc))
        data->status |= UPS_ST_INPUT_BAD;

    if (read_status_bit(t, HID_PAGE_BATTERY, HID_USAGE_BAT_REMAINING_TIME_EXP, buf, rc))
        data->sig_status |= UPS_SIG_SHUTDOWN_IMMINENT;

    if (read_status_bit(t, HID_PAGE_POWER, HID_USAGE_COMMUNICATIONS_LOST, buf, rc))
        data->general_error |= UPS_GENERR_INTERNAL_COMM;

    if (read_status_bit(t, HID_PAGE_BATTERY, HID_USAGE_BAT_NEED_REPLACEMENT, buf, rc))
        data->bat_system_error |= UPS_BATERR_REPLACE;

    /* Only flag "battery disconnected" when the descriptor actually
     * exposes BatteryPresent AND it reads false. CyberPower CP1500PFCLCD
     * (and likely others) omit the field entirely on units that don't
     * support runtime battery removal; without this guard, every poll
     * would set DISCONNECTED and trigger a false alert on startup. */
    if (has_status_bit(t, HID_PAGE_BATTERY, HID_USAGE_BAT_BATTERY_PRESENT) &&
        !read_status_bit(t, HID_PAGE_BATTERY, HID_USAGE_BAT_BATTERY_PRESENT, buf, rc))
        data->bat_system_error |= UPS_BATERR_DISCONNECTED;

    if (read_status_bit(t, HID_PAGE_BATTERY, HID_USAGE_BAT_VOLTAGE_NOT_REG, buf, rc))
        data->status |= UPS_ST_FAULT;

    /* AVR Boost/Buck bits — opt-out via t->skip_avr_bits. CyberPower
     * PowerPanel HID units (CP1500PFCLCD and friends) expose these
     * usages but they don't track a real AVR tap-changer (the unit is
     * line-interactive in the PFC sinewave sense, not the AVR sense),
     * and per NUT's CPS subdriver they read stuck-on. Vendors with
     * reliable AVR signalling (APC) leave the flag at 0. */
    if (!t->skip_avr_bits) {
        if (read_status_bit(t, HID_PAGE_POWER, HID_USAGE_BOOST, buf, rc))
            data->status |= UPS_ST_AVR_BOOST;
        if (read_status_bit(t, HID_PAGE_POWER, HID_USAGE_BUCK, buf, rc))
            data->status |= UPS_ST_AVR_TRIM;
    }

    /* --- Test field state --- */
    if (t->pdc.test) {
        int32_t test_val = hid_field_read_raw(t->fd, t->pdc.test);
        /* Read values per HID PDC spec: 1=pass, 2=warning, 3=error,
         * 4=aborted, 5=in progress, 6=none. Vendor firmware sometimes
         * remaps these — the raw value is exposed via bat_test_status. */
        if (test_val == 5)
            data->status |= UPS_ST_TEST;
        data->bat_test_status = (uint16_t)test_val;
    }

    /* Synthesise outlet state from online/on-battery presence */
    if (data->status & UPS_ST_ONLINE)
        data->outlet_mog = (1u << 0); /* on */
    else if (data->status & UPS_ST_ON_BATTERY)
        data->outlet_mog = (1u << 0); /* still on, running from battery */
    else
        data->outlet_mog = (1u << 1); /* off */

    return 0;
}

int hid_pdc_read_dynamic_standard(hid_pdc_transport_t *t, ups_data_t *data)
{
    data->charge_pct       = hid_field_read_scaled(t->fd, t->pdc.remaining_capacity);
    data->battery_voltage  = hid_field_read_scaled(t->fd, t->pdc.battery_voltage);
    data->runtime_sec      = (uint32_t)hid_field_read_scaled(t->fd, t->pdc.runtime_to_empty);
    data->input_voltage    = hid_field_read_scaled(t->fd, t->pdc.input_voltage);

    /* Output voltage: no dedicated sensor in standard HID PDC. On battery
     * the device runs at its nominal config voltage; online it passes
     * input through. Accurate for standby/line-interactive units; not
     * perfect during transitional states. */
    if (data->status & UPS_ST_ON_BATTERY)
        data->output_voltage = hid_field_read_scaled(t->fd, t->pdc.output_voltage);
    else
        data->output_voltage = data->input_voltage;

    data->load_pct         = hid_field_read_scaled(t->fd, t->pdc.percent_load);

    /* HID PDC has no required frequency usage and many consumer Back-UPS /
     * CyberPower SKUs simply don't expose HID_USAGE_FREQUENCY. NaN signals
     * "unavailable" so the API can omit the field and the UI can hide the
     * metric, rather than rendering a misleading 0.00 Hz reading. */
    if (t->pdc.output_frequency)
        data->output_frequency = hid_field_read_scaled(t->fd, t->pdc.output_frequency);
    else
        data->output_frequency = (double)NAN;

    if (data->output_voltage > 0 && t->nominal_watts > 0)
        data->output_current = (data->load_pct / 100.0)
                             * (double)t->nominal_watts
                             / data->output_voltage;
    else
        data->output_current = 0;

    if (t->pdc.delay_before_shutdown) {
        int32_t v = hid_field_read_raw(t->fd, t->pdc.delay_before_shutdown);
        data->timer_shutdown = (int16_t)v;
    }
    if (t->pdc.delay_before_startup) {
        int32_t v = hid_field_read_raw(t->fd, t->pdc.delay_before_startup);
        data->timer_start = (int16_t)v;
    }
    if (t->pdc.delay_before_reboot) {
        int32_t v = hid_field_read_raw(t->fd, t->pdc.delay_before_reboot);
        data->timer_reboot = (int32_t)v;
    }

    /* No bypass or efficiency in standard HID PDC */
    data->bypass_voltage    = 0;
    data->bypass_frequency  = (double)NAN;
    data->efficiency        = 0.0;
    data->efficiency_reason = UPS_EFF_NOT_AVAILABLE;

    return 0;
}

int hid_pdc_read_thresholds_standard(hid_pdc_transport_t *t,
                                      uint16_t *transfer_high,
                                      uint16_t *transfer_low)
{
    *transfer_high = (uint16_t)hid_field_read_scaled(t->fd, t->pdc.transfer_high);
    *transfer_low  = (uint16_t)hid_field_read_scaled(t->fd, t->pdc.transfer_low);
    return 0;
}

/* --- Standard commands -------------------------------------------------- */

int hid_pdc_cmd_shutdown(hid_pdc_transport_t *t)
{
    if (!t->pdc.delay_before_shutdown) return -1;

    /* Per APC AN178: write DelayBeforeStartup first so the UPS
     * automatically restores output when AC returns, then write
     * DelayBeforeShutdown to begin the shutdown countdown. Without the
     * startup write, the UPS stays off permanently after the shutdown
     * fires. CyberPower units honour the same sequence per NUT. */
    if (t->pdc.delay_before_startup) {
        uint8_t sbuf[3] = { t->pdc.delay_before_startup->report_id, 0, 0 };
        hid_pdc_set_feature(t->fd, sbuf[0], sbuf, sizeof(sbuf));
    }

    uint8_t buf[3] = { t->pdc.delay_before_shutdown->report_id, 60, 0 };
    return hid_pdc_set_feature(t->fd, buf[0], buf, sizeof(buf)) >= 0 ? 0 : -1;
}

int hid_pdc_cmd_abort_shutdown(hid_pdc_transport_t *t)
{
    if (!t->pdc.delay_before_shutdown) return -1;
    /* 0xFFFF (-1) per HID PDC spec — abort any pending shutdown. */
    uint8_t buf[3] = { t->pdc.delay_before_shutdown->report_id, 0xFF, 0xFF };
    return hid_pdc_set_feature(t->fd, buf[0], buf, sizeof(buf)) >= 0 ? 0 : -1;
}

int hid_pdc_cmd_battery_test(hid_pdc_transport_t *t)
{
    if (!t->pdc.test) return -1;
    /* Test=1 → quick self-test. Test=2 (deep) is intentionally not
     * exposed; many consumer units accept the write but immediately
     * return state=4 (aborted) without entering state=5 (in progress). */
    uint8_t buf[2] = { t->pdc.test->report_id, 0x01 };
    return hid_pdc_set_feature(t->fd, buf[0], buf, sizeof(buf)) >= 0 ? 0 : -1;
}

int hid_pdc_cmd_mute_alarm(hid_pdc_transport_t *t)
{
    if (!t->pdc.alarm_control) return -1;
    uint8_t buf[2] = { t->pdc.alarm_control->report_id, 0x03 };
    return hid_pdc_set_feature(t->fd, buf[0], buf, sizeof(buf)) >= 0 ? 0 : -1;
}

int hid_pdc_cmd_unmute_alarm(hid_pdc_transport_t *t)
{
    if (!t->pdc.alarm_control) return -1;
    uint8_t buf[2] = { t->pdc.alarm_control->report_id, 0x02 };
    return hid_pdc_set_feature(t->fd, buf[0], buf, sizeof(buf)) >= 0 ? 0 : -1;
}

int hid_pdc_cmd_clear_faults(hid_pdc_transport_t *t)
{
    if (!t->pdc.module_reset) return -1;
    /* ModuleReset=2 → reset module's alarms */
    uint8_t buf[2] = { t->pdc.module_reset->report_id, 0x02 };
    return hid_pdc_set_feature(t->fd, buf[0], buf, sizeof(buf)) >= 0 ? 0 : -1;
}

/* --- Generic config_read / config_write --------------------------------- */

int hid_pdc_config_read_field(int fd,
                               const hid_field_t *field,
                               const ups_config_reg_t *reg,
                               uint32_t *raw_value)
{
    if (!field) return UPS_ERR_NOT_SUPPORTED;

    int32_t val = hid_field_read_raw(fd, field);

    /* For bitfield registers with multi-byte HID fields (e.g., APC
     * sensitivity is 24-bit but only the low byte holds the enum),
     * mask to the byte that contains the value. */
    if (reg->type == UPS_CFG_BITFIELD && field->bit_size > 8)
        val &= 0xFF;

    if (raw_value) *raw_value = (uint32_t)val;
    return UPS_OK;
}

int hid_pdc_config_write_field(int fd,
                                const hid_field_t *field,
                                const ups_config_reg_t *reg,
                                uint16_t value)
{
    if (!field) return UPS_ERR_NOT_SUPPORTED;

    uint16_t data_bytes = (uint16_t)((field->bit_size + 7u) / 8u);
    uint8_t buf[8] = { 0 };
    buf[0] = field->report_id;

    if (reg->type == UPS_CFG_BITFIELD && data_bytes > 1) {
        /* Multi-byte bitfield: read current report, modify only the
         * value byte, preserve the rest. */
        uint8_t cur[8] = { 0 };
        cur[0] = field->report_id;
        uint16_t rlen = (uint16_t)(1u + data_bytes);
        if (ioctl(fd, HIDIOCGFEATURE(rlen), cur) < 0)
            return UPS_ERR_IO;
        memcpy(buf, cur, rlen);
        buf[1] = (uint8_t)value;
    } else {
        buf[1] = (uint8_t)(value & 0xFF);
        if (data_bytes > 1)
            buf[2] = (uint8_t)((value >> 8) & 0xFF);
    }

    uint16_t wlen = (uint16_t)(1u + data_bytes);
    if (hid_pdc_set_feature(fd, buf[0], buf, wlen) < 0)
        return UPS_ERR_IO;

    /* Read back and verify */
    uint32_t readback = 0;
    if (hid_pdc_config_read_field(fd, field, reg, &readback) != UPS_OK)
        return UPS_ERR_IO;
    return (readback == value) ? UPS_OK : UPS_ERR_IO;
}
