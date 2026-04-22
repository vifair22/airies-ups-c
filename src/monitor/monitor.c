#include "monitor/monitor.h"
#include "monitor/status_snapshot.h"
#include <cutils/log.h>
#include <cutils/error.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_EVENT_CBS 4
#define HE_REENGAGE_THRESHOLD 30

/* --- Monitor state --- */

struct monitor {
    ups_t        *ups;
    cutils_db_t  *db;
    int           poll_sec;
    int           telemetry_sec;

    /* Thread */
    pthread_t     thread;
    volatile int  running;

    /* Current state (protected by mutex) */
    pthread_mutex_t mutex;
    ups_data_t      data;
    ups_inventory_t inventory;
    int             has_data;
    int             connected;

    /* State change detection. `snapshot` is loaded from SQLite at
     * thread start so transitions are detected against the daemon's
     * last known state (catches faults that became active while the
     * daemon was offline). prev_status mirrors snapshot.status for
     * the existing transition-detection block; the wider snapshot is
     * also handed to the alert engine on init for the same reason. */
    uint64_t          prev_sig;
    uint32_t          prev_status;
    status_snapshot_t snapshot;
    int               first_poll;

    /* Daily config snapshot */
    time_t        last_config_snapshot;

    /* Connectivity tracking */
    int           was_connected;
    time_t        disconnected_at;

    /* HE inhibit tracking */
    int           he_inhibit;
    char          he_source[32];
    int           he_reengage_count;

    /* Telemetry timing */
    time_t        last_telemetry;

    /* Retention */
    retention_config_t retention;
    int                retention_enabled;
    time_t             last_retention;

    /* Event callbacks */
    monitor_event_fn event_fns[MAX_EVENT_CBS];
    void            *event_uds[MAX_EVENT_CBS];
    int              nevent_cbs;

    /* Per-poll callbacks */
    monitor_poll_fn  poll_fns[MAX_EVENT_CBS];
    void            *poll_uds[MAX_EVENT_CBS];
    int              npoll_cbs;
};

/* --- Helpers --- */

static uint64_t status_signature(const ups_data_t *d)
{
    return ((uint64_t)d->status << 32) |
           ((uint64_t)d->sig_status << 16) |
           ((uint64_t)d->transfer_reason);
}

static void fire_event(monitor_t *mon, const char *severity,
                       const char *category, const char *title,
                       const char *message)
{
    /* Write to event journal */
    char ts[32];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

    const char *params[] = { ts, severity, category, title, message, NULL };
    db_execute_non_query(mon->db,
        "INSERT INTO events (timestamp, severity, category, title, message) "
        "VALUES (?, ?, ?, ?, ?)",
        params, NULL);

    /* Fire callbacks */
    for (int i = 0; i < mon->nevent_cbs; i++) {
        if (mon->event_fns[i])
            mon->event_fns[i](severity, category, title, message,
                              mon->event_uds[i]);
    }
}

static void record_telemetry(monitor_t *mon, const ups_data_t *d)
{
    char ts[32];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

    char status_s[16], charge_s[16], runtime_s[16], bv_s[16];
    char load_s[16], ov_s[16], of_s[16], oc_s[16], iv_s[16], eff_s[16];
    char mog_s[16], sog0_s[16], sog1_s[16], energy_s[16];

    snprintf(status_s, sizeof(status_s), "%u", d->status);
    snprintf(charge_s, sizeof(charge_s), "%.1f", d->charge_pct);
    snprintf(runtime_s, sizeof(runtime_s), "%u", d->runtime_sec);
    snprintf(bv_s, sizeof(bv_s), "%.1f", d->battery_voltage);
    snprintf(load_s, sizeof(load_s), "%.1f", d->load_pct);
    snprintf(ov_s, sizeof(ov_s), "%.1f", d->output_voltage);
    snprintf(of_s, sizeof(of_s), "%.2f", d->output_frequency);
    snprintf(oc_s, sizeof(oc_s), "%.1f", d->output_current);
    snprintf(iv_s, sizeof(iv_s), "%.1f", d->input_voltage);
    snprintf(eff_s, sizeof(eff_s), "%.1f", d->efficiency);
    snprintf(mog_s, sizeof(mog_s), "%u", d->outlet_mog);
    snprintf(sog0_s, sizeof(sog0_s), "%u", d->outlet_sog0);
    snprintf(sog1_s, sizeof(sog1_s), "%u", d->outlet_sog1);
    snprintf(energy_s, sizeof(energy_s), "%u", d->output_energy_wh);

    const char *params[] = {
        ts, status_s, charge_s, runtime_s, bv_s, load_s,
        ov_s, of_s, oc_s, iv_s, eff_s,
        mog_s, sog0_s, sog1_s, energy_s, NULL
    };
    db_execute_non_query(mon->db,
        "INSERT INTO telemetry (timestamp, status, charge_pct, runtime_sec, "
        "battery_voltage, load_pct, output_voltage, output_frequency, "
        "output_current, input_voltage, efficiency, "
        "outlet_mog, outlet_sog0, outlet_sog1, output_energy_wh) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        params, NULL);
}

static void format_status_line(const ups_data_t *d, char *buf, size_t len)
{
    char status_str[256], eff_str[64];
    ups_status_str(d->status, status_str, sizeof(status_str));
    ups_efficiency_str((int)d->efficiency_reason, d->efficiency, eff_str, sizeof(eff_str));

    snprintf(buf, len,
             "Status: %s | Chg: %.0f%% | Rt: %um%us | Bv: %.1fV | "
             "In: %.1fV | Out: %.1fV %.1fHz %.1fA | Load: %.0f%% | "
             "Eff: %s | Xfer: %s",
             status_str, d->charge_pct,
             d->runtime_sec / 60, d->runtime_sec % 60,
             d->battery_voltage, d->input_voltage,
             d->output_voltage, d->output_frequency, d->output_current,
             d->load_pct, eff_str,
             ups_transfer_reason_str(d->transfer_reason));
}

/* --- Config register snapshot --- */

static void snapshot_config_registers(monitor_t *mon)
{
    size_t count;
    const ups_config_reg_t *regs = ups_get_config_regs(mon->ups, &count);
    if (!regs || count == 0) return;

    char ts[32];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

    int snapped = 0;
    for (size_t i = 0; i < count; i++) {
        uint16_t raw = 0;
        if (ups_config_read(mon->ups, &regs[i], &raw, NULL, 0) != 0)
            continue;

        char raw_s[16], display_s[64];
        snprintf(raw_s, sizeof(raw_s), "%u", raw);
        if (regs[i].scale > 1)
            snprintf(display_s, sizeof(display_s), "%.1f%s%s",
                     (double)raw / regs[i].scale,
                     regs[i].unit ? " " : "", regs[i].unit ? regs[i].unit : "");
        else
            snprintf(display_s, sizeof(display_s), "%u%s%s",
                     raw,
                     regs[i].unit ? " " : "", regs[i].unit ? regs[i].unit : "");

        const char *params[] = { regs[i].name, raw_s, display_s, ts, NULL };
        db_execute_non_query(mon->db,
            "INSERT INTO ups_config (register_name, raw_value, display_value, timestamp) "
            "VALUES (?, ?, ?, ?)",
            params, NULL);
        snapped++;
    }

    if (snapped > 0)
        log_info("config snapshot: recorded %d registers", snapped);
}

/* --- Monitor thread --- */

static void *monitor_thread(void *arg)
{
    monitor_t *mon = arg;
    ups_data_t data;

    /* Use cached inventory from ups_connect() */
    if (mon->ups->has_inventory) {
        pthread_mutex_lock(&mon->mutex);
        mon->inventory = mon->ups->inventory;
        pthread_mutex_unlock(&mon->mutex);
        log_info("UPS: %s | Serial: %s | FW: %s | %uVA / %uW | Driver: %s",
                 mon->inventory.model, mon->inventory.serial,
                 mon->inventory.firmware,
                 mon->inventory.nominal_va, mon->inventory.nominal_watts,
                 ups_driver_name(mon->ups));
    } else {
        log_warn("UPS inventory not available");
    }

    /* mon->snapshot was loaded by monitor_create() before this thread
     * started, so transition detection has a valid baseline regardless
     * of whether the initial UPS read below succeeds. */
    mon->prev_status = mon->snapshot.status;
    mon->first_poll  = 0;

    /* Initial status read */
    if (ups_read_status(mon->ups, &data) == 0 &&
        ups_read_dynamic(mon->ups, &data) == 0) {
        pthread_mutex_lock(&mon->mutex);
        mon->data = data;
        mon->has_data = 1;
        mon->connected = 1;
        mon->prev_sig = status_signature(&data);
        pthread_mutex_unlock(&mon->mutex);

        char line[512];
        format_status_line(&data, line, sizeof(line));
        log_info("%s", line);

        fire_event(mon, "info", "system", "Monitor Started", line);
        mon->was_connected = 1;
    }

    mon->last_telemetry = time(NULL);

    /* Poll loop */
    while (mon->running) {
        sleep((unsigned int)mon->poll_sec);
        if (!mon->running) break;

        /* Read UPS */
        memset(&data, 0, sizeof(data));
        if (ups_read_status(mon->ups, &data) != 0 ||
            ups_read_dynamic(mon->ups, &data) != 0) {
            log_error("failed to read UPS data");
            pthread_mutex_lock(&mon->mutex);
            mon->connected = 0;
            pthread_mutex_unlock(&mon->mutex);

            /* Fire disconnect event on transition */
            if (mon->was_connected) {
                mon->was_connected = 0;
                mon->disconnected_at = time(NULL);
                fire_event(mon, "error", "system", "UPS Disconnected",
                           "Lost communication with UPS");
            }
            continue;
        }

        pthread_mutex_lock(&mon->mutex);
        mon->data = data;
        mon->has_data = 1;
        mon->connected = 1;
        pthread_mutex_unlock(&mon->mutex);

        /* Fire reconnect event on transition */
        if (!mon->was_connected && mon->disconnected_at > 0) {
            time_t gap = time(NULL) - mon->disconnected_at;
            char msg[128];
            if (gap >= 3600)
                snprintf(msg, sizeof(msg),
                         "UPS communication restored after %ldh %ldm",
                         gap / 3600, (gap % 3600) / 60);
            else if (gap >= 60)
                snprintf(msg, sizeof(msg),
                         "UPS communication restored after %ldm %lds",
                         gap / 60, gap % 60);
            else
                snprintf(msg, sizeof(msg),
                         "UPS communication restored after %lds", gap);
            fire_event(mon, "warning", "system", "UPS Reconnected", msg);
            mon->disconnected_at = 0;
        }
        mon->was_connected = 1;

        /* Fire per-poll callbacks (alert engine runs here) */
        for (int i = 0; i < mon->npoll_cbs; i++) {
            if (mon->poll_fns[i])
                mon->poll_fns[i](&data, mon->poll_uds[i]);
        }

        /* HE inhibit auto-clear */
        if (mon->he_inhibit && ups_has_cap(mon->ups, UPS_CAP_HE_MODE)) {
            if (data.status & UPS_ST_HE_MODE) {
                mon->he_reengage_count++;
                if (mon->he_reengage_count >= HE_REENGAGE_THRESHOLD) {
                    log_info("UPS stably re-engaged HE mode, clearing inhibit");
                    monitor_he_inhibit_clear(mon);
                    fire_event(mon, "info", "status",
                               "HE Mode Restored",
                               "HE mode re-engaged, inhibit cleared");
                }
            } else {
                if (mon->he_reengage_count > 0)
                    mon->he_reengage_count = 0;
            }
        }

        /* State change detection — per-bit transition events */
        if (!mon->first_poll) {
            uint32_t changed = mon->prev_status ^ data.status;
            uint32_t set     = changed & data.status;
            uint32_t cleared = changed & mon->prev_status;
            char msg[512];

            if (changed) {
                format_status_line(&data, msg, sizeof(msg));
                log_info("%s", msg);
            }

            /* Power events */
            if (set & UPS_ST_ON_BATTERY) {
                snprintf(msg, sizeof(msg),
                         "UPS switched to battery — charge %.0f%%, runtime %um%us",
                         data.charge_pct, data.runtime_sec / 60,
                         data.runtime_sec % 60);
                fire_event(mon, "warning", "power", "On Battery", msg);
            }
            if (cleared & UPS_ST_ON_BATTERY)
                fire_event(mon, "info", "power", "Utility Restored",
                           "UPS returned to utility power");
            if (set & UPS_ST_OUTPUT_OFF)
                fire_event(mon, "error", "power", "Output Off",
                           "UPS output has been turned off");
            if (cleared & UPS_ST_OUTPUT_OFF)
                fire_event(mon, "info", "power", "Output Restored",
                           "UPS output has been restored");

            /* Mode events */
            if (set & UPS_ST_HE_MODE)
                fire_event(mon, "info", "mode", "HE Mode Entered",
                           "UPS entered high efficiency mode");
            if (cleared & UPS_ST_HE_MODE)
                fire_event(mon, "info", "mode", "HE Mode Exited",
                           "UPS exited high efficiency mode");
            if (set & UPS_ST_BYPASS)
                fire_event(mon, "warning", "mode", "Bypass Entered",
                           (data.status & UPS_ST_COMMANDED)
                               ? "UPS entered commanded bypass — output is unprotected"
                               : "UPS transferred to bypass due to internal fault");
            if (cleared & UPS_ST_BYPASS)
                fire_event(mon, "info", "mode", "Bypass Exited",
                           "UPS returned to normal operation from bypass");

            /* Fault events */
            if (set & (UPS_ST_FAULT | UPS_ST_FAULT_STATE))
                fire_event(mon, "error", "fault", "Fault Detected",
                           "UPS has entered a fault condition");
            if (cleared & (UPS_ST_FAULT | UPS_ST_FAULT_STATE))
                fire_event(mon, "info", "fault", "Fault Cleared",
                           "UPS fault condition has been cleared");
            if (set & UPS_ST_OVERLOAD)
                fire_event(mon, "error", "fault", "Overload",
                           "UPS output is overloaded");
            if (cleared & UPS_ST_OVERLOAD)
                fire_event(mon, "info", "fault", "Overload Cleared",
                           "UPS overload condition has cleared");

            /* Test events */
            if (set & UPS_ST_TEST)
                fire_event(mon, "info", "test", "Self-Test Started",
                           "UPS self-test is in progress");
            if (cleared & UPS_ST_TEST)
                fire_event(mon, "info", "test", "Self-Test Ended",
                           "UPS self-test has completed");

            /* Shutdown trigger: on battery + shutdown imminent */
            if ((data.status & UPS_ST_ON_BATTERY) &&
                (data.sig_status & UPS_SIG_SHUTDOWN_IMMINENT) &&
                !(mon->prev_status & UPS_ST_ON_BATTERY &&
                  /* only fire once on transition */
                  (mon->data.sig_status & UPS_SIG_SHUTDOWN_IMMINENT))) {
                snprintf(msg, sizeof(msg),
                         "Battery: %.0f%% Runtime: %um%us — shutdown triggered",
                         data.charge_pct, data.runtime_sec / 60,
                         data.runtime_sec % 60);
                log_error("SHUTDOWN TRIGGERED: %s", msg);
                fire_event(mon, "critical", "shutdown",
                           "Shutdown Triggered", msg);
            }

            mon->prev_status = data.status;
        }

        /* Persist any change in tracked diff fields to SQLite. Write
         * only on change so stable systems generate no DB traffic and
         * the on-disk row always reflects the last observed transition.
         * Snapshot is reloaded by both the monitor and the alert engine
         * at the next daemon startup. */
        if (data.status              != mon->snapshot.status              ||
            data.bat_system_error    != mon->snapshot.bat_system_error    ||
            data.general_error       != mon->snapshot.general_error       ||
            data.power_system_error  != mon->snapshot.power_system_error  ||
            data.bat_lifetime_status != mon->snapshot.bat_lifetime_status) {
            mon->snapshot.status              = data.status;
            mon->snapshot.bat_system_error    = data.bat_system_error;
            mon->snapshot.general_error       = data.general_error;
            mon->snapshot.power_system_error  = data.power_system_error;
            mon->snapshot.bat_lifetime_status = data.bat_lifetime_status;
            status_snapshot_save(mon->db, &mon->snapshot);
        }

        /* Also track the full signature for transfer reason changes */
        uint64_t sig = status_signature(&data);
        mon->prev_sig = sig;

        /* Telemetry recording (downsampled) */
        time_t now = time(NULL);
        if (now - mon->last_telemetry >= mon->telemetry_sec) {
            record_telemetry(mon, &data);
            mon->last_telemetry = now;
        }

        /* Daily retention cleanup */
        if (mon->retention_enabled && now - mon->last_retention >= 86400) {
            retention_run(mon->db, &mon->retention);
            mon->last_retention = now;
        }

        /* Daily config register snapshot (catches LCD-initiated changes) */
        if (now - mon->last_config_snapshot >= 86400) {
            snapshot_config_registers(mon);
            mon->last_config_snapshot = now;
        }
    }

    return NULL;
}

/* --- Public API --- */

monitor_t *monitor_create(ups_t *ups, cutils_db_t *db,
                          int poll_interval_sec, int telemetry_interval_sec)
{
    monitor_t *mon = calloc(1, sizeof(*mon));
    if (!mon) return NULL;

    mon->ups = ups;
    mon->db = db;
    mon->poll_sec = poll_interval_sec > 0 ? poll_interval_sec : 2;
    mon->telemetry_sec = telemetry_interval_sec > 0 ? telemetry_interval_sec : 30;
    mon->first_poll = 1;

    pthread_mutex_init(&mon->mutex, NULL);

    /* Load persisted status snapshot synchronously so callers that need
     * it before monitor_start (e.g. main.c seeding the alert engine via
     * alerts_seed_from_snapshot) see the loaded state, not zeros. If no
     * snapshot exists (fresh install / DB wipe), seed a baseline of
     * UPS_ST_ONLINE so a healthy first boot is silent — anything else
     * active on first poll fires events as if newly transitioned. */
    if (status_snapshot_load(db, &mon->snapshot) != 0) {
        memset(&mon->snapshot, 0, sizeof(mon->snapshot));
        mon->snapshot.status = UPS_ST_ONLINE;
        log_info("status snapshot: no prior state — baseline = UPS_ST_ONLINE");
    } else {
        log_info("status snapshot: loaded prior state "
                 "(status=0x%08x, bat_err=0x%04x, gen_err=0x%04x, "
                 "pwr_err=0x%08x, lifetime=0x%04x, updated %s)",
                 mon->snapshot.status, mon->snapshot.bat_system_error,
                 mon->snapshot.general_error, mon->snapshot.power_system_error,
                 mon->snapshot.bat_lifetime_status, mon->snapshot.updated_at);
    }

    return mon;
}

int monitor_on_event(monitor_t *mon, monitor_event_fn fn, void *userdata)
{
    if (mon->nevent_cbs >= MAX_EVENT_CBS)
        return set_error(CUTILS_ERR, "too many event callbacks");
    mon->event_fns[mon->nevent_cbs] = fn;
    mon->event_uds[mon->nevent_cbs] = userdata;
    mon->nevent_cbs++;
    return CUTILS_OK;
}

void monitor_set_retention(monitor_t *mon, const retention_config_t *cfg)
{
    mon->retention = *cfg;
    mon->retention_enabled = 1;
    mon->last_retention = time(NULL);
    mon->last_config_snapshot = 0;  /* force snapshot on first pass */
}

int monitor_on_poll(monitor_t *mon, monitor_poll_fn fn, void *userdata)
{
    if (mon->npoll_cbs >= MAX_EVENT_CBS)
        return set_error(CUTILS_ERR, "too many poll callbacks");
    mon->poll_fns[mon->npoll_cbs] = fn;
    mon->poll_uds[mon->npoll_cbs] = userdata;
    mon->npoll_cbs++;
    return CUTILS_OK;
}

int monitor_start(monitor_t *mon)
{
    mon->running = 1;
    int rc = pthread_create(&mon->thread, NULL, monitor_thread, mon);
    if (rc != 0) {
        mon->running = 0;
        return set_error(CUTILS_ERR, "failed to create monitor thread");
    }
    log_info("monitor started (poll %ds, telemetry %ds)",
             mon->poll_sec, mon->telemetry_sec);
    return CUTILS_OK;
}

void monitor_stop(monitor_t *mon)
{
    if (!mon) return;
    mon->running = 0;
    pthread_join(mon->thread, NULL);

    /* Belt-and-suspenders: write a final snapshot reflecting the very
     * last observed state. The on-change writes inside the poll loop
     * already cover the practical failure modes; this only narrows the
     * one-poll-cycle window where a status change observed in the loop's
     * last iteration could be lost if the daemon was killed before the
     * next save would have fired. Cheap insurance — single UPSERT. */
    if (mon->has_data) {
        status_snapshot_t final = mon->snapshot;
        final.status              = mon->data.status;
        final.bat_system_error    = mon->data.bat_system_error;
        final.general_error       = mon->data.general_error;
        final.power_system_error  = mon->data.power_system_error;
        final.bat_lifetime_status = mon->data.bat_lifetime_status;
        status_snapshot_save(mon->db, &final);
    }

    pthread_mutex_destroy(&mon->mutex);
    free(mon);
}

/* --- Thread-safe state accessors --- */

int monitor_get_status(monitor_t *mon, ups_data_t *out)
{
    pthread_mutex_lock(&mon->mutex);
    if (!mon->has_data) {
        pthread_mutex_unlock(&mon->mutex);
        return -1;
    }
    *out = mon->data;
    pthread_mutex_unlock(&mon->mutex);
    return 0;
}

int monitor_get_inventory(monitor_t *mon, ups_inventory_t *out)
{
    pthread_mutex_lock(&mon->mutex);
    *out = mon->inventory;
    pthread_mutex_unlock(&mon->mutex);
    return 0;
}

int monitor_get_snapshot(monitor_t *mon, status_snapshot_t *out)
{
    pthread_mutex_lock(&mon->mutex);
    *out = mon->snapshot;
    pthread_mutex_unlock(&mon->mutex);
    return 0;
}

const char *monitor_driver_name(monitor_t *mon)
{
    return ups_driver_name(mon->ups);
}

int monitor_is_connected(monitor_t *mon)
{
    return mon->connected;
}

int monitor_he_inhibit_active(monitor_t *mon)
{
    return mon->he_inhibit;
}

const char *monitor_he_inhibit_source(monitor_t *mon)
{
    return mon->he_source;
}

void monitor_he_inhibit_set(monitor_t *mon, const char *source)
{
    mon->he_inhibit = 1;
    snprintf(mon->he_source, sizeof(mon->he_source), "%s", source);
}

void monitor_fire_event(monitor_t *mon, const char *severity,
                        const char *category, const char *title,
                        const char *message)
{
    fire_event(mon, severity, category, title, message);
}

void monitor_he_inhibit_clear(monitor_t *mon)
{
    mon->he_inhibit = 0;
    mon->he_source[0] = '\0';
    mon->he_reengage_count = 0;
}
