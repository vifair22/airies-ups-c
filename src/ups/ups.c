#include "ups.h"
#include "ups_format.h"
#include <cutils/mem.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- Driver registry --- */

extern const ups_driver_t ups_driver_srt;
extern const ups_driver_t ups_driver_smt;
extern const ups_driver_t ups_driver_apc_hid;
extern const ups_driver_t ups_driver_cyberpower_hid;

const ups_driver_t *ups_drivers[] = {
    &ups_driver_srt,
    &ups_driver_smt,
    &ups_driver_apc_hid,
    &ups_driver_cyberpower_hid,
    NULL,
};

/* --- Runtime subset resolution ---
 *
 * Both initial connect and reconnect need to narrow the driver's static
 * capability mask and config_regs array to what actually resolved against
 * the live device. Factored into one helper so the reconnect path can't
 * drift from the connect path. */
static void ups_resolve_runtime(ups_t *ups, void *transport)
{
    const ups_driver_t *drv = ups->driver;

    ups->caps = drv->caps;
    if (drv->resolve_caps)
        ups->caps = drv->resolve_caps(transport, drv->caps);

    /* Drop any previous resolved set (matters on reconnect). */
    free(ups->resolved_regs);
    ups->resolved_regs       = NULL;
    ups->resolved_regs_count = 0;

    if (drv->resolve_config_regs && drv->config_regs && drv->config_regs_count > 0) {
        ups_config_reg_t *buf = calloc(drv->config_regs_count, sizeof(*buf));
        if (buf) {
            size_t n = drv->resolve_config_regs(transport,
                                                drv->config_regs,
                                                drv->config_regs_count,
                                                buf);
            if (n == 0) {
                free(buf);
            } else {
                ups->resolved_regs       = buf;
                ups->resolved_regs_count = n;
            }
        }
    }
}

/* --- Connection + auto-detect ---
 *
 * For each registered driver matching the connection type:
 *   1. Call driver->connect(params) to open the transport
 *   2. Call driver->detect(transport) to check if this UPS matches
 *   3. If detect returns 1, we have our driver. Otherwise disconnect and try next.
 */

int ups_connect(const ups_conn_params_t *params, ups_t **out)
{
    if (!params || !out) return UPS_ERR_IO;
    *out = NULL;

    /* Distinguish three failure modes:
     *   - No driver registered for this conn_type     → UPS_ERR_NO_DRIVER
     *   - Matching driver(s), but connect() always failed → UPS_ERR_IO
     *   - connect() succeeded but no detect() claimed it  → UPS_ERR_NO_DRIVER
     */
    int any_matched_type = 0;
    int any_connected    = 0;

    for (int i = 0; ups_drivers[i] != NULL; i++) {
        const ups_driver_t *drv = ups_drivers[i];

        if (drv->conn_type != params->type)
            continue;
        any_matched_type = 1;

        if (!drv->connect || !drv->detect)
            continue;

        void *transport = drv->connect(params);
        if (!transport)
            continue;
        any_connected = 1;

        if (!drv->detect(transport)) {
            if (drv->disconnect) drv->disconnect(transport);
            continue;
        }

        /* Match — build the context */
        ups_t *ups = calloc(1, sizeof(*ups));
        if (!ups) {
            if (drv->disconnect) drv->disconnect(transport);
            return UPS_ERR_IO;
        }

        ups->transport = transport;
        ups->driver = drv;
        ups->params = *params;
        /* Deep-copy string pointers so params survive the caller's stack */
        if (params->type == UPS_CONN_SERIAL && params->serial.device) {
            snprintf(ups->_device_buf, sizeof(ups->_device_buf), "%s", params->serial.device);
            ups->params.serial.device = ups->_device_buf;
        }
        if (params->type == UPS_CONN_USB && params->usb.serial) {
            snprintf(ups->_serial_buf, sizeof(ups->_serial_buf), "%s", params->usb.serial);
            ups->params.usb.serial = ups->_serial_buf;
        }
        if (params->type == UPS_CONN_TCP && params->tcp.host) {
            snprintf(ups->_host_buf, sizeof(ups->_host_buf), "%s", params->tcp.host);
            ups->params.tcp.host = ups->_host_buf;
        }
        if (params->type == UPS_CONN_SNMP && params->snmp.host) {
            snprintf(ups->_host_buf, sizeof(ups->_host_buf), "%s", params->snmp.host);
            ups->params.snmp.host = ups->_host_buf;
        }

        pthread_mutex_init(&ups->cmd_mutex, NULL);

        /* Resolve effective capabilities + config registers. Drivers may
         * narrow both based on what actually resolved against the live
         * device (missing HID fields, Modbus exception codes, etc.). */
        ups_resolve_runtime(ups, transport);

        /* Read inventory immediately while connection is fresh */
        if (ups_read_inventory(ups, &ups->inventory) == 0)
            ups->has_inventory = 1;

        *out = ups;
        return UPS_OK;
    }

    if (!any_matched_type) return UPS_ERR_NO_DRIVER;
    if (!any_connected)    return UPS_ERR_IO;
    return UPS_ERR_NO_DRIVER;
}

void ups_close(ups_t *ups)
{
    if (ups) {
        if (ups->transport && ups->driver->disconnect)
            ups->driver->disconnect(ups->transport);
        pthread_mutex_destroy(&ups->cmd_mutex);
        free(ups->resolved_regs);
        free(ups);
    }
}

/* --- Capability queries --- */

int ups_has_cap(const ups_t *ups, ups_cap_t cap)
{
    return ups && (ups->caps & (uint32_t)cap) != 0;
}

const char *ups_driver_name(const ups_t *ups)
{
    return ups->driver->name;
}

int ups_is_connected(const ups_t *ups)
{
    return ups && ups->transport != NULL;
}

ups_topology_t ups_topology(const ups_t *ups)
{
    if (ups->driver->get_topology)
        return ups->driver->get_topology(ups->transport);
    return ups->driver->topology;
}

const ups_freq_setting_t *ups_get_freq_settings(const ups_t *ups, size_t *count)
{
    if (!ups_has_cap(ups, UPS_CAP_FREQ_TOLERANCE) || !ups->driver->freq_settings) {
        if (count) *count = 0;
        return NULL;
    }
    if (count) *count = ups->driver->freq_settings_count;
    return ups->driver->freq_settings;
}

const ups_freq_setting_t *ups_find_freq_setting(const ups_t *ups, const char *name)
{
    size_t count;
    const ups_freq_setting_t *settings = ups_get_freq_settings(ups, &count);
    if (!settings) return NULL;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(settings[i].name, name) == 0)
            return &settings[i];
    }
    return NULL;
}

const ups_freq_setting_t *ups_find_freq_value(const ups_t *ups, uint16_t value)
{
    size_t count;
    const ups_freq_setting_t *settings = ups_get_freq_settings(ups, &count);
    if (!settings) return NULL;
    for (size_t i = 0; i < count; i++) {
        if (settings[i].value == value)
            return &settings[i];
    }
    return NULL;
}

/* --- Error recovery ---
 *
 * Two-phase recovery: repeated read failures tear down the transport
 * (ups_handle_error), and any subsequent read attempts reconnect
 * (ups_try_reconnect). The retry path never "gives up" — a USB serial
 * device that disappears briefly is expected to come back, and the
 * monitor loop will keep invoking reads that drive retries.
 *
 * RECONNECT_INTERVAL_SEC rate-limits reconnects so we don't try every
 * single poll while the port is down. It is NOT a giving-up timeout. */

#define MAX_CONSECUTIVE_ERRORS 5
#define RECONNECT_INTERVAL_SEC 2

static void ups_clear_errors(ups_t *ups)
{
    ups->consecutive_errors = 0;
}

/* Attempt to (re)open the transport if it's currently down.
 *
 * Rate-limited to one attempt per RECONNECT_INTERVAL_SEC so a fast poll
 * loop doesn't hammer the driver's connect path.
 *
 * Reconnect runs the same post-connect steps as initial ups_connect():
 * detect() to guard against a different device appearing on the same
 * port, resolve_caps() because the new device may expose a different
 * subset, and read_inventory() because cached identity fields may be
 * stale. If detect() fails (different UPS on the wire now), we tear the
 * transport back down and leave ups->transport NULL so the next tick
 * retries from scratch. */
static void ups_try_reconnect(ups_t *ups)
{
    if (ups->transport) return;
    if (!ups->driver->connect) return;

    time_t now = time(NULL);
    if (now - ups->last_reconnect_attempt < RECONNECT_INTERVAL_SEC) return;
    ups->last_reconnect_attempt = now;

    void *transport = ups->driver->connect(&ups->params);
    if (!transport) return;

    /* Guard against a different device on the wire. */
    if (ups->driver->detect && !ups->driver->detect(transport)) {
        if (ups->driver->disconnect) ups->driver->disconnect(transport);
        return;
    }

    ups->transport = transport;
    ups->consecutive_errors = 0;

    /* Re-narrow caps and config_regs; a different-generation device (or
     * a replaced UPS of the same family) may expose a different subset. */
    ups_resolve_runtime(ups, transport);

    /* Refresh cached inventory; otherwise model/serial/firmware stay
     * frozen at the original connect's reading. read_inventory may fail
     * transiently — leave the previous cache in place if so. */
    if (ups->driver->read_inventory) {
        ups_inventory_t fresh = {0};
        if (ups->driver->read_inventory(transport, &fresh) == 0) {
            ups->inventory = fresh;
            ups->has_inventory = 1;
        }
    }
}

/* Tear down the transport after repeated read failures. The next read
 * call will reopen via ups_try_reconnect. */
static void ups_handle_error(ups_t *ups)
{
    ups->consecutive_errors++;

    if (ups->consecutive_errors >= MAX_CONSECUTIVE_ERRORS) {
        if (ups->transport && ups->driver->disconnect)
            ups->driver->disconnect(ups->transport);
        ups->transport = NULL;
        ups->last_reconnect_attempt = 0;  /* allow immediate first retry */
    }
}

/* Shared prologue for driver calls: if transport is down, try to bring
 * it back. Returns 1 if transport is usable, 0 if caller should bail. */
static int ups_ensure_transport(ups_t *ups)
{
    if (!ups->transport) ups_try_reconnect(ups);
    return ups->transport != NULL;
}

/* --- Read dispatch --- */

int ups_read_status(ups_t *ups, ups_data_t *data)
{
    if (!ups->driver->read_status) return UPS_ERR_NOT_SUPPORTED;
    CUTILS_LOCK_GUARD(&ups->cmd_mutex);
    if (!ups_ensure_transport(ups)) return UPS_ERR_IO;
    int rc = ups->driver->read_status(ups->transport, data);
    if (rc != 0) ups_handle_error(ups); else ups_clear_errors(ups);
    return rc;
}

int ups_read_dynamic(ups_t *ups, ups_data_t *data)
{
    if (!ups->driver->read_dynamic) return UPS_ERR_NOT_SUPPORTED;

    int rc;
    {
        CUTILS_LOCK_GUARD(&ups->cmd_mutex);
        if (!ups_ensure_transport(ups)) return UPS_ERR_IO;
        rc = ups->driver->read_dynamic(ups->transport, data);
        if (rc != 0) ups_handle_error(ups); else ups_clear_errors(ups);
    }
    /* Lock released; battery-zero fixup below can run unlocked since
     * `data` is caller-owned and not shared with other threads. */

    /* Zero battery readings when battery is disconnected — the UPS reports
     * stale values and the frontend shouldn't display them */
    if (rc == 0 && (data->bat_system_error & UPS_BATERR_DISCONNECTED)) {
        data->charge_pct = 0;
        data->battery_voltage = 0;
        data->runtime_sec = 0;
    }

    return rc;
}

int ups_read_inventory(ups_t *ups, ups_inventory_t *inv)
{
    if (!ups->driver->read_inventory) return UPS_ERR_NOT_SUPPORTED;
    CUTILS_LOCK_GUARD(&ups->cmd_mutex);
    if (!ups_ensure_transport(ups)) return UPS_ERR_IO;
    return ups->driver->read_inventory(ups->transport, inv);
}

int ups_read_thresholds(ups_t *ups, uint16_t *transfer_high, uint16_t *transfer_low)
{
    if (!ups->driver->read_thresholds) return UPS_ERR_NOT_SUPPORTED;
    CUTILS_LOCK_GUARD(&ups->cmd_mutex);
    if (!ups_ensure_transport(ups)) return UPS_ERR_IO;
    return ups->driver->read_thresholds(ups->transport, transfer_high, transfer_low);
}

/* --- Command dispatch ---
 * All commands hold the mutex. Post-command settle delay gives the UPS
 * time to process before the monitor's next read. */

static void post_command_settle(void)
{
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 200000000 }; /* 200ms */
    nanosleep(&ts, NULL);
}

/* --- Command descriptor access --- */

const ups_cmd_desc_t *ups_get_commands(const ups_t *ups, size_t *count)
{
    if (!ups->driver->commands) {
        if (count) *count = 0;
        return NULL;
    }
    if (count) *count = ups->driver->commands_count;
    return ups->driver->commands;
}

const ups_cmd_desc_t *ups_find_command(const ups_t *ups, const char *name)
{
    size_t count;
    const ups_cmd_desc_t *cmds = ups_get_commands(ups, &count);
    if (!cmds) return NULL;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(cmds[i].name, name) == 0)
            return &cmds[i];
    }
    return NULL;
}

const ups_cmd_desc_t *ups_find_command_flag(const ups_t *ups, uint32_t flag)
{
    size_t count;
    const ups_cmd_desc_t *cmds = ups_get_commands(ups, &count);
    if (!cmds) return NULL;
    for (size_t i = 0; i < count; i++) {
        if (cmds[i].flags & flag)
            return &cmds[i];
    }
    return NULL;
}

int ups_cmd_execute(ups_t *ups, const char *name, int is_off)
{
    const ups_cmd_desc_t *cmd = ups_find_command(ups, name);
    if (!cmd) return UPS_ERR_NOT_SUPPORTED;

    int (*fn)(void *) = is_off ? cmd->execute_off : cmd->execute;
    if (!fn) return UPS_ERR_NOT_SUPPORTED;

    CUTILS_LOCK_GUARD(&ups->cmd_mutex);
    if (!ups_ensure_transport(ups)) return UPS_ERR_IO;
    int rc = fn(ups->transport);
    post_command_settle();
    return rc;
}

/* --- Config register access ---
 * Delegates to driver's config_read/config_write if provided.
 * Falls back to UPS_ERR_NOT_SUPPORTED if the driver doesn't implement them. */

const ups_config_reg_t *ups_get_config_regs(const ups_t *ups, size_t *count)
{
    /* Prefer the resolved subset if the driver narrowed; otherwise fall
     * back to the driver's static array verbatim. */
    if (ups->resolved_regs) {
        if (count) *count = ups->resolved_regs_count;
        return ups->resolved_regs;
    }
    if (!ups->driver->config_regs) {
        if (count) *count = 0;
        return NULL;
    }
    if (count) *count = ups->driver->config_regs_count;
    return ups->driver->config_regs;
}

const ups_config_reg_t *ups_find_config_reg(const ups_t *ups, const char *name)
{
    size_t count;
    const ups_config_reg_t *regs = ups_get_config_regs(ups, &count);
    if (!regs) return NULL;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(regs[i].name, name) == 0)
            return &regs[i];
    }
    return NULL;
}

int ups_config_read(ups_t *ups, const ups_config_reg_t *reg,
                    uint32_t *raw_value, char *str_buf, size_t str_bufsz)
{
    if (!ups->driver->config_read) return UPS_ERR_NOT_SUPPORTED;
    CUTILS_LOCK_GUARD(&ups->cmd_mutex);
    if (!ups_ensure_transport(ups)) return UPS_ERR_IO;
    int rc = ups->driver->config_read(ups->transport, reg, raw_value, str_buf, str_bufsz);
    if (rc != 0) ups_handle_error(ups); else ups_clear_errors(ups);
    return rc;
}

/* Validate a write against the descriptor's metadata before letting it
 * reach the driver. Keeps invalid values off the wire entirely (some
 * UPS firmwares respond to out-of-range writes with a control-plane
 * reset; see docs/reference/apc-srt-modbus.md "Rapid-fire config register
 * writes" note) and gives the API layer a clean operator-error signal
 * distinct from real I/O failures.
 *
 * Returns UPS_OK if the value is acceptable to enqueue; otherwise
 * UPS_ERR_INVALID_VALUE. The contract is documented next to the meta
 * union in ups_driver.h. */
static int ups_config_validate(const ups_config_reg_t *reg, uint16_t value)
{
    switch (reg->type) {
    case UPS_CFG_SCALAR: {
        int32_t v   = (int32_t)value;
        int32_t min = reg->meta.scalar.min;
        int32_t max = reg->meta.scalar.max;
        /* Convention: min == 0 && max == 0 means "no range declared" */
        if (min == 0 && max == 0) return UPS_OK;
        if (v < min || v > max)   return UPS_ERR_INVALID_VALUE;
        return UPS_OK;
    }
    case UPS_CFG_BITFIELD: {
        if (!reg->meta.bitfield.strict) return UPS_OK;
        for (size_t i = 0; i < reg->meta.bitfield.count; i++)
            if (reg->meta.bitfield.opts[i].value == value)
                return UPS_OK;
        return UPS_ERR_INVALID_VALUE;
    }
    case UPS_CFG_STRING:
        /* String writes don't go through this path; the string-write
         * helper enforces max_chars at its own boundary. */
        return UPS_OK;
    case UPS_CFG_FLAGS:
        /* FLAGS descriptors are read-only diagnostics (status bits,
         * error registers); no driver currently writes them. If/when a
         * future driver does, validation should ensure the value covers
         * only declared bits — until then, accept and let the driver
         * reject if needed. */
        return UPS_OK;
    }
    return UPS_OK;
}

int ups_config_write(ups_t *ups, const ups_config_reg_t *reg, uint16_t value)
{
    if (!reg->writable) return UPS_ERR_NOT_SUPPORTED;
    if (!ups->driver->config_write) return UPS_ERR_NOT_SUPPORTED;

    int vrc = ups_config_validate(reg, value);
    if (vrc != UPS_OK) return vrc;

    CUTILS_LOCK_GUARD(&ups->cmd_mutex);
    if (!ups_ensure_transport(ups)) return UPS_ERR_IO;
    int rc = ups->driver->config_write(ups->transport, reg, value);
    post_command_settle();
    return rc;
}

