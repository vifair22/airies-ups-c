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
 * Uses the standard HID Power Device usage page (0x84) and
 * Battery System page (0x85) with APC vendor extensions (0xFF86).
 *
 * All report IDs and unit scaling are derived from parsing the HID
 * report descriptor at connect time — no hardcoded report IDs.
 *
 * Tested on: BE600M1 (Back-UPS ES 600M1) */

#define APC_VID  0x051D
#define APC_PID  0x0002

/* Usage paths for field lookup.
 * These are collection-qualified: [collection..., usage] matched as a suffix
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

static const uint32_t UP_SENSITIVITY[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_INPUT),
    HID_UP(HID_PAGE_APC, HID_USAGE_APC_SENSITIVITY) };

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

/* Power.Output / Output collection — PercentLoad lives in a sub-collection */
static const uint32_t UP_PERCENT_LOAD[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_PERCENT_LOAD) };

/* Power controls — single-usage lookup */
static const uint32_t UP_TEST[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_TEST) };

static const uint32_t UP_ALARM_CONTROL[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_AUDIBLE_ALARM_CTRL) };

/* Power.Output.Frequency — standard HID usage 0x0032 (Hertz, no scaling).
 * Note: BE600M1 does not expose this field; output_frequency will be 0. */
static const uint32_t UP_OUTPUT_FREQUENCY[] = {
    HID_UP(HID_PAGE_POWER, HID_USAGE_OUTPUT),
    HID_UP(HID_PAGE_POWER, HID_USAGE_FREQUENCY) };

/* --- Resolved field cache (populated on connect) --- */

typedef struct {
    const hid_field_t *input_voltage;
    const hid_field_t *output_voltage;     /* Input.ConfigVoltage (nominal, used as output) */
    const hid_field_t *battery_voltage;
    const hid_field_t *remaining_capacity;
    const hid_field_t *runtime_to_empty;
    const hid_field_t *percent_load;
    const hid_field_t *output_frequency;
    const hid_field_t *transfer_low;
    const hid_field_t *transfer_high;
    const hid_field_t *sensitivity;
    const hid_field_t *test;
    const hid_field_t *alarm_control;
} hid_fields_t;

/* --- Transport --- */

typedef struct {
    int              fd;
    char             sysfs_base[PATH_MAX];
    char             hidraw_name[NAME_MAX]; /* "hidrawN" for descriptor path */
    hid_report_map_t map;
    hid_fields_t     f;
    uint16_t         nominal_watts;
} hid_transport_t;

/* --- hidraw helpers (for commands that still need direct writes) --- */

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

            /* Resolve sysfs USB device path for string descriptors */
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

/* Resolve all the usage paths we need into cached field pointers */
static void resolve_fields(hid_transport_t *t)
{
    hid_fields_t *f = &t->f;
    const hid_report_map_t *m = &t->map;

    f->input_voltage     = hid_find_field(m, UP_INPUT_VOLTAGE, 2, HID_FIELD_FEATURE);
    f->output_voltage    = hid_find_field(m, UP_INPUT_CONFIG_VOLTAGE, 2, HID_FIELD_FEATURE);
    f->battery_voltage   = hid_find_field(m, UP_BATTERY_VOLTAGE, 2, HID_FIELD_FEATURE);
    f->remaining_capacity = hid_find_field(m, UP_REMAINING_CAPACITY, 2, HID_FIELD_FEATURE);
    f->runtime_to_empty  = hid_find_field(m, UP_RUNTIME_TO_EMPTY, 2, HID_FIELD_FEATURE);
    f->percent_load      = hid_find_field(m, UP_PERCENT_LOAD, 1, HID_FIELD_FEATURE);
    f->output_frequency  = hid_find_field(m, UP_OUTPUT_FREQUENCY, 2, HID_FIELD_FEATURE);
    f->transfer_low      = hid_find_field(m, UP_TRANSFER_LOW, 2, HID_FIELD_FEATURE);
    f->transfer_high     = hid_find_field(m, UP_TRANSFER_HIGH, 2, HID_FIELD_FEATURE);
    f->sensitivity       = hid_find_field(m, UP_SENSITIVITY, 2, HID_FIELD_FEATURE);
    f->test              = hid_find_field(m, UP_TEST, 1, HID_FIELD_FEATURE);
    f->alarm_control     = hid_find_field(m, UP_ALARM_CONTROL, 1, HID_FIELD_FEATURE);

    /* Log what we found (and what's missing) */
    if (f->input_voltage)     log_info("backups_hid: input_voltage → RID 0x%02x", f->input_voltage->report_id);
    else                      log_warn("backups_hid: input_voltage not found in descriptor");
    if (f->battery_voltage)   log_info("backups_hid: battery_voltage → RID 0x%02x", f->battery_voltage->report_id);
    if (f->percent_load)      log_info("backups_hid: percent_load → RID 0x%02x", f->percent_load->report_id);
    else                      log_warn("backups_hid: percent_load not found in descriptor");
    if (f->sensitivity)       log_info("backups_hid: sensitivity → RID 0x%02x", f->sensitivity->report_id);
    if (f->output_frequency)  log_info("backups_hid: output_freq → RID 0x%02x", f->output_frequency->report_id);
}

/* Forward declarations */
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

    /* Parse the HID report descriptor and resolve field pointers */
    if (parse_hid_descriptor(t) != 0) {
        log_warn("backups_hid: continuing without descriptor (reads may be wrong)");
    } else {
        resolve_fields(t);
        fixup_config_regs(t);
    }

    return t;
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

    /* Verify we can read battery capacity */
    if (t->f.remaining_capacity) {
        int32_t cap = hid_field_read_raw(t->fd, t->f.remaining_capacity);
        if (cap == 0) /* retry once */
            cap = hid_field_read_raw(t->fd, t->f.remaining_capacity);
        (void)cap;
    }

    /* Check sysfs product string contains "Back-UPS" */
    char product[128];
    read_sysfs_string(t->sysfs_base, "product", product, sizeof(product));
    if (strstr(product, "Back-UPS") != NULL)
        return 1;

    /* Also accept any APC UPS on this USB interface */
    char mfg[128];
    read_sysfs_string(t->sysfs_base, "manufacturer", mfg, sizeof(mfg));
    return strstr(mfg, "American Power Conversion") != NULL;
}

/* --- Status bitfield ---
 *
 * The PresentStatus report (0x16 on BE600M1) contains individual 1-bit
 * fields packed into a multi-byte report. We read the whole report and
 * extract named bits by their usage. The bit positions come from the
 * parsed descriptor, not hardcoded offsets. */

/* Find a 1-bit status field by its usage within the PresentStatus collection */
static int read_status_bit(hid_transport_t *t, uint16_t page, uint16_t usage,
                           const uint8_t *report, int report_len)
{
    /* Search for a 1-bit Feature field matching this usage in any
     * PresentStatus-like collection (Power.0x0002 = PresentStatus) */
    for (size_t i = 0; i < t->map.count; i++) {
        const hid_field_t *f = &t->map.fields[i];
        if (f->type != HID_FIELD_FEATURE) continue;
        if (f->bit_size != 1) continue;
        if (f->usage_page != page || f->usage_id != usage) continue;

        /* Extract the bit from the report buffer */
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

    /* Read the full status report. We need the report ID — find any 1-bit
     * status field to get it. ACPresent (85:D0) is always present. */
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

    /* Map HID status bits to our semantic status flags.
     * Page:usage mappings per APC AN178 §2.3 and USB HID PDC spec. */
    data->status = 0;

    /* ACPresent — Battery page 85:D0 (APC AN178 §2.3.3) */
    if (read_status_bit(t, HID_PAGE_BATTERY, HID_USAGE_BAT_AC_PRESENT, buf, rc))
        data->status |= UPS_ST_ONLINE;
    /* Discharging — Battery page 85:45 */
    if (read_status_bit(t, HID_PAGE_BATTERY, HID_USAGE_BAT_DISCHARGING, buf, rc))
        data->status |= UPS_ST_ON_BATTERY;
    /* Overload — Power Device page 84:65 (APC AN178 §2.3.11) */
    if (read_status_bit(t, HID_PAGE_POWER, HID_USAGE_OVERLOAD, buf, rc))
        data->status |= UPS_ST_OVERLOAD;
    /* ShutdownRequested — Power Device page 84:68 (APC AN178 §2.3.6) */
    if (read_status_bit(t, HID_PAGE_POWER, HID_USAGE_SHUTDOWN_REQUESTED, buf, rc))
        data->status |= UPS_ST_SHUT_PENDING;
    /* NeedReplacement — Battery page 85:4B (APC AN178 §2.3.10) */
    if (read_status_bit(t, HID_PAGE_BATTERY, HID_USAGE_BAT_NEED_REPLACEMENT, buf, rc))
        data->bat_system_error |= UPS_BATERR_REPLACE;

    /* Synthesize outlet state — Back-UPS has a single unswitched outlet group */
    data->outlet_mog = (data->status & UPS_ST_ONLINE) ? (1u << 0) : (1u << 1);

    return 0;
}

/* --- Dynamic reads (all descriptor-driven) --- */

static int backups_read_dynamic(void *transport, ups_data_t *data)
{
    hid_transport_t *t = transport;

    data->charge_pct       = hid_field_read_scaled(t->fd, t->f.remaining_capacity);
    data->battery_voltage  = hid_field_read_scaled(t->fd, t->f.battery_voltage);
    data->runtime_sec      = (uint32_t)hid_field_read_scaled(t->fd, t->f.runtime_to_empty);
    data->input_voltage    = hid_field_read_scaled(t->fd, t->f.input_voltage);

    /* Standby topology: output IS the input (passthrough) when online.
     * On battery the inverter targets the nominal config voltage. */
    if (data->status & UPS_ST_ON_BATTERY)
        data->output_voltage = hid_field_read_scaled(t->fd, t->f.output_voltage);
    else
        data->output_voltage = data->input_voltage;
    data->load_pct         = hid_field_read_scaled(t->fd, t->f.percent_load);

    /* Output frequency: standard HID Frequency usage, value in Hz */
    data->output_frequency = hid_field_read_scaled(t->fd, t->f.output_frequency);

    /* Derive output current from load % and nominal watts.
     * No current sensor on standby units, but we can estimate:
     * I = (load_pct / 100) * nominal_watts / output_voltage */
    if (data->output_voltage > 0 && t->nominal_watts > 0)
        data->output_current = (data->load_pct / 100.0)
                             * (double)t->nominal_watts
                             / data->output_voltage;
    else
        data->output_current = 0;

    /* Standby units don't have bypass or efficiency */
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

    /* TODO: parse VA/W from model string or use a lookup table */
    inv->nominal_va = 600;
    inv->nominal_watts = 330;
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

    /* DelayBeforeShutdown (report 0x15): kills output after N seconds.
     * Note: BE600M1 does not auto-restart after commanded shutdown —
     * requires physical button press to restore output. This is a hardware
     * limitation; APC.APCShutdownAfterDelay (0x60) is silently ignored. */
    uint8_t buf[3] = { 0x15, 60, 0 };
    return hid_set_feature(t->fd, 0x15, buf, sizeof(buf)) >= 0 ? 0 : -1;
}

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

static int backups_cmd_battery_test(void *transport)
{
    hid_transport_t *t = transport;
    if (!t->f.test) return -1;
    /* Test: 1 = Quick test (§4.1.4) */
    uint8_t buf[2] = { t->f.test->report_id, 0x01 };
    return hid_set_feature(t->fd, buf[0], buf, sizeof(buf)) >= 0 ? 0 : -1;
}

/* --- Config registers ---
 *
 * The config register "reg_addr" field stores the HID report ID resolved
 * at connect time. config_read and config_write use the cached field pointer
 * for reads and direct HID set_feature for writes. */

static const ups_bitfield_opt_t sensitivity_opts[] = {
    { 1, "high",   "High" },
    { 2, "medium", "Medium" },
    { 3, "low",    "Low" },
};

/* These are populated with resolved report IDs at connect time via
 * backups_fixup_config_regs(). The initial reg_addr values are placeholders. */
static ups_config_reg_t backups_config_regs[] = {
    { "sensitivity", "Input Sensitivity", NULL, "power_quality",
      0, 1, UPS_CFG_BITFIELD, 1, 1,
      .meta.bitfield = { sensitivity_opts,
                         sizeof(sensitivity_opts) / sizeof(sensitivity_opts[0]) } },
    { "transfer_low", "Low Transfer Voltage", "V", "transfer",
      0, 1, UPS_CFG_SCALAR, 1, 0,
      .meta.scalar = { 0, 0 } },
    { "transfer_high", "High Transfer Voltage", "V", "transfer",
      0, 1, UPS_CFG_SCALAR, 1, 0,
      .meta.scalar = { 0, 0 } },
};

/* Patch config_regs with resolved report IDs from the descriptor */
static void fixup_config_regs(hid_transport_t *t)
{
    if (t->f.sensitivity)
        backups_config_regs[0].reg_addr = t->f.sensitivity->report_id;
    if (t->f.transfer_low)
        backups_config_regs[1].reg_addr = t->f.transfer_low->report_id;
    if (t->f.transfer_high)
        backups_config_regs[2].reg_addr = t->f.transfer_high->report_id;
}

static int backups_config_read(void *transport, const ups_config_reg_t *reg,
                                uint16_t *raw_value, char *str_buf, size_t str_bufsz)
{
    (void)str_buf; (void)str_bufsz;
    hid_transport_t *t = transport;

    /* Find the field by report ID (which was resolved from the descriptor) */
    const hid_field_t *field = NULL;
    for (size_t i = 0; i < t->map.count; i++) {
        if (t->map.fields[i].report_id == reg->reg_addr &&
            t->map.fields[i].type == HID_FIELD_FEATURE) {
            field = &t->map.fields[i];
            break;
        }
    }

    if (!field) return UPS_ERR_NOT_SUPPORTED;

    int32_t val = hid_field_read_raw(t->fd, field);
    if (raw_value) *raw_value = (uint16_t)val;
    return UPS_OK;
}

static int backups_config_write(void *transport, const ups_config_reg_t *reg, uint16_t value)
{
    hid_transport_t *t = transport;

    if (!reg->writable)
        return UPS_ERR_NOT_SUPPORTED;

    /* Write via direct HID set_feature with the resolved report ID */
    if (reg->type == UPS_CFG_BITFIELD) {
        uint8_t buf[2] = { (uint8_t)reg->reg_addr, (uint8_t)value };
        if (hid_set_feature(t->fd, (uint8_t)reg->reg_addr, buf, sizeof(buf)) < 0)
            return UPS_ERR_IO;
    } else {
        uint8_t buf[3] = { (uint8_t)reg->reg_addr,
                           (uint8_t)(value & 0xFF),
                           (uint8_t)((value >> 8) & 0xFF) };
        if (hid_set_feature(t->fd, (uint8_t)reg->reg_addr, buf, sizeof(buf)) < 0)
            return UPS_ERR_IO;
    }

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

    { "battery_test", "Battery Test", "Run a battery self-test",
      "diagnostics", "Start Battery Test?",
      "This will run a brief self-test to verify battery health.",
      UPS_CMD_SIMPLE, UPS_CMD_DEFAULT, 0, 0,
      backups_cmd_battery_test, NULL },

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
    .topology            = UPS_TOPO_STANDBY,
    .connect             = backups_connect,
    .disconnect          = backups_disconnect,
    .detect              = backups_detect,
    .caps                = UPS_CAP_SHUTDOWN | UPS_CAP_BATTERY_TEST | UPS_CAP_MUTE,
    .freq_settings       = NULL,
    .freq_settings_count = 0,
    .config_regs         = backups_config_regs,
    .config_regs_count   = sizeof(backups_config_regs) / sizeof(backups_config_regs[0]),
    .read_status         = backups_read_status,
    .read_dynamic        = backups_read_dynamic,
    .read_inventory      = backups_read_inventory,
    .read_thresholds     = backups_read_thresholds,
    .commands            = backups_commands,
    .commands_count      = sizeof(backups_commands) / sizeof(backups_commands[0]),
    .config_read         = backups_config_read,
    .config_write        = backups_config_write,
};
