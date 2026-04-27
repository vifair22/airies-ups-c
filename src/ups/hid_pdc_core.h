#ifndef HID_PDC_CORE_H
#define HID_PDC_CORE_H

/* USB HID Power Device Class core — vendor-neutral plumbing.
 *
 * Wraps the standard HID PDC contract (usage pages 0x84 / 0x85 per the
 * USB-IF HID PDC v1.1 spec) so vendor-specific drivers (APC, CyberPower,
 * etc.) only have to declare their VID/PID list, identification strings,
 * and any vendor-page extensions.
 *
 * Vendor adapter responsibilities:
 *   1. Provide a `connect` that VID/PID-prechecks the params, then calls
 *      hid_pdc_open(). Stash any vendor-page field pointers on
 *      transport->vendor.
 *   2. Provide a `detect` that confirms identity (manufacturer/product
 *      sysfs strings) using hid_pdc_read_sysfs_string().
 *   3. Provide a `disconnect` that frees vendor state then calls
 *      hid_pdc_close().
 *   4. Implement the `ups_driver_t` reads/commands/config_* callbacks,
 *      generally by delegating to hid_pdc_* helpers and adding any
 *      vendor-specific behaviour on top.
 *
 * Everything keyed by HID usage path is resolved at connect time by
 * parsing the device's report descriptor — no hardcoded report IDs. */

#include "hid_parser.h"
#include "ups_driver.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

/* (vid, pid) match entry. Vendor adapters declare a static list and
 * precheck params before calling hid_pdc_open(). */
typedef struct {
    uint16_t vid;
    uint16_t pid;
} hid_pdc_vid_pid_t;

/* Resolved standard PDC fields. Any pointer may be NULL if the device's
 * report descriptor doesn't expose that usage. Vendor adapters narrow
 * their advertised caps and config_regs accordingly. */
typedef struct {
    /* Live measurements */
    const hid_field_t *input_voltage;
    const hid_field_t *output_voltage;        /* Input.ConfigVoltage (nominal) */
    const hid_field_t *battery_voltage;
    const hid_field_t *remaining_capacity;
    const hid_field_t *runtime_to_empty;
    const hid_field_t *percent_load;
    const hid_field_t *output_frequency;

    /* Thresholds / nameplate */
    const hid_field_t *transfer_low;
    const hid_field_t *transfer_high;
    const hid_field_t *config_apparent_power;
    const hid_field_t *config_active_power;

    /* Power controls */
    const hid_field_t *test;
    const hid_field_t *alarm_control;
    const hid_field_t *delay_before_shutdown;
    const hid_field_t *delay_before_startup;
    const hid_field_t *delay_before_reboot;
    const hid_field_t *module_reset;

    /* Battery settings / info */
    const hid_field_t *remaining_time_limit;
    const hid_field_t *remaining_cap_limit;
    const hid_field_t *warning_cap_limit;
    const hid_field_t *design_capacity;
    const hid_field_t *manufacture_date;
} hid_pdc_fields_t;

/* The transport handle vendor adapters cast `void *transport` to. */
typedef struct {
    int               fd;
    char              sysfs_base[PATH_MAX];
    char              hidraw_name[NAME_MAX];
    hid_report_map_t  map;
    hid_pdc_fields_t  pdc;             /* standard PDC field cache */
    void             *vendor;          /* vendor-owned cached state; vendor frees */
    uint16_t          nominal_watts;   /* from inventory; used for output current calc */
    ups_topology_t    topology;        /* auto-detected from PowerConverter presence */
    /* Vendor-controlled toggles for status bits whose meaning isn't
     * universal across HID PDC implementations. Default 0 = trust the
     * spec; vendor's connect() bumps these when it knows a bit is
     * unreliable on the live device family. */
    int               skip_avr_bits;   /* 1 = ignore Boost/Buck (e.g. CyberPower) */
} hid_pdc_transport_t;

/* ---- Lifecycle ---------------------------------------------------------- */

/* Open the hidraw device matching params->usb.{vendor_id,product_id}, parse
 * its report descriptor, resolve standard PDC fields, and auto-detect
 * topology. Returns a heap-allocated transport on success, NULL on any
 * failure (no hidraw match, open failure, descriptor parse failure).
 *
 * Vendor adapters typically call this from their connect() after a quick
 * VID/PID precheck against their claimed list. */
hid_pdc_transport_t *hid_pdc_open(const ups_conn_params_t *params);

/* Close the transport opened by hid_pdc_open(). Caller must free
 * transport->vendor (and any nested vendor state) before calling. */
void hid_pdc_close(hid_pdc_transport_t *t);

/* ---- Generic helpers (also useful to vendor adapters) ------------------- */

/* Wrapper around HIDIOCSFEATURE. Returns ioctl rc (>=0 on success). */
int hid_pdc_set_feature(int fd, uint8_t report_id, const void *buf, size_t len);

/* Read /sys/class/hidraw/<name>/device/<attr> into buf. Strips trailing
 * newline. Sets buf[0]='\0' on any failure. Used by detect() and
 * read_inventory() to read sysfs strings (manufacturer, product, serial). */
void hid_pdc_read_sysfs_string(const char *base, const char *attr,
                                char *buf, size_t bufsz);

/* Look up a field by its index into map->fields[]. Vendor adapters stash
 * this index in their config descriptor's reg_addr at fixup time, which
 * sidesteps the "multiple fields share a report ID" ambiguity that a
 * RID-only lookup would hit (e.g. CyberPower's CP1500PFCLCD packs three
 * battery-capacity descriptors into RID 0x07). Returns NULL if index is
 * out of range. */
const hid_field_t *hid_pdc_field_by_index(const hid_report_map_t *map,
                                           uint16_t index);

/* Stash the field's location (index in map->fields[]) into a config
 * descriptor's reg_addr, plus the logical min/max into the scalar meta
 * if applicable. Vendor adapters call this from their fixup function for
 * each (descriptor, resolved-field) pair. No-op if field is NULL. */
void hid_pdc_fixup_config_reg(ups_config_reg_t *reg,
                               const hid_field_t *field,
                               const hid_report_map_t *map);

/* Convert the standard HID PDC battery ManufactureDate encoding
 * ((year-1980)*512 + month*32 + day) to days since 2000-01-01 so the
 * shared API date formatter renders ISO dates identically across drivers. */
uint16_t hid_pdc_usb_date_to_days_since_2000(uint16_t raw);

/* ---- Standard PDC reads ------------------------------------------------- */

/* Populate ups_data_t fields covered by the standard HID PDC PresentStatus
 * collection and the Test field. Clears status/sig_status/general_error/
 * bat_system_error/transfer_reason then ORs in resolved bits. Synthesises
 * outlet_mog from ONLINE/ON_BATTERY presence. Returns 0 on success,
 * UPS_ERR_IO on ioctl failure. Vendor adapter can OR in vendor-page bits
 * after this returns. */
int hid_pdc_read_status_standard(hid_pdc_transport_t *t, ups_data_t *data);

/* Populate ups_data_t measurements from the standard fields. Derives
 * output_current from load% and nominal_watts. Zeroes bypass_voltage/
 * frequency and sets efficiency_reason = NOT_AVAILABLE (HID PDC has
 * no notion of efficiency). Returns 0. */
int hid_pdc_read_dynamic_standard(hid_pdc_transport_t *t, ups_data_t *data);

/* Read transfer thresholds in volts (matches the public API's volts
 * contract, not the raw register units). Returns 0. */
int hid_pdc_read_thresholds_standard(hid_pdc_transport_t *t,
                                      uint16_t *transfer_high,
                                      uint16_t *transfer_low);

/* ---- Standard commands -------------------------------------------------- */

/* Per APC AN178 (interoperable across HID PDC vendors): write
 * DelayBeforeStartup=0 first so the UPS auto-restores when AC returns,
 * then DelayBeforeShutdown=60 to begin the countdown. */
int hid_pdc_cmd_shutdown(hid_pdc_transport_t *t);

/* Write 0xFFFF to DelayBeforeShutdown to abort any pending shutdown. */
int hid_pdc_cmd_abort_shutdown(hid_pdc_transport_t *t);

/* Write 1 to Test to begin a quick self-test. */
int hid_pdc_cmd_battery_test(hid_pdc_transport_t *t);

/* Write 3 (Muted) to AudibleAlarmControl. */
int hid_pdc_cmd_mute_alarm(hid_pdc_transport_t *t);

/* Write 2 (Enabled) to AudibleAlarmControl. */
int hid_pdc_cmd_unmute_alarm(hid_pdc_transport_t *t);

/* Write 2 (Reset Module's Alarms) to ModuleReset. */
int hid_pdc_cmd_clear_faults(hid_pdc_transport_t *t);

/* ---- Generic config_read / config_write --------------------------------- */

/* Generic scalar/bitfield reader keyed by a resolved HID field. Writes the
 * raw value (after low-byte mask for multi-byte bitfields) into *raw_value.
 * Returns UPS_OK or UPS_ERR_NOT_SUPPORTED if field is NULL. */
int hid_pdc_config_read_field(int fd,
                               const hid_field_t *field,
                               const ups_config_reg_t *reg,
                               uint32_t *raw_value);

/* Generic scalar/bitfield writer keyed by a resolved HID field. For
 * multi-byte bitfields, reads the existing report and modifies only the
 * low byte (vendor enums fit in 8 bits). Verifies write by readback.
 * Returns UPS_OK on success, UPS_ERR_IO on transport failure or readback
 * mismatch, UPS_ERR_NOT_SUPPORTED if field is NULL. */
int hid_pdc_config_write_field(int fd,
                                const hid_field_t *field,
                                const ups_config_reg_t *reg,
                                uint16_t value);

#endif /* HID_PDC_CORE_H */
