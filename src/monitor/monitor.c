#include "monitor/monitor.h"
#include "monitor/status_snapshot.h"
#include "monitor/config_snapshot.h"
#include "monitor/xfer_ring.h"
#include <cutils/log.h>
#include <cutils/error.h>
#include <cutils/mem.h>

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_EVENT_CBS 4
#define HE_REENGAGE_THRESHOLD 30

/* --- Transfer-reason fast-poll cadence ---
 *
 * Cadence the fast-poll thread runs at, and the lookback window the slow
 * poll uses when annotating events with a recent cause. See xfer_ring.h
 * for the rationale (brief mains glitches resolve before the slow poll
 * lands; we capture transitions in real time on a 200 ms cadence and
 * search a 5 s window when a status-bit transition fires). */
#define XFER_FAST_POLL_MS   200
#define XFER_LOOKBACK_MS    5000

/* --- Monitor state --- */

struct monitor {
    ups_t        *ups;
    cutils_db_t  *db;
    int           poll_sec;

    /* Thread */
    pthread_t     thread;
    volatile int  running;

    /* Transfer-reason fast-poll thread (only spawned when the active
     * driver implements read_transfer_reason — HID drivers leave it
     * NULL and skip this entirely). The ring buffer stores recent
     * transitions of register 2 so event annotations can pick up
     * causes that flickered on/off between slow polls. */
    pthread_t       xfer_thread;
    volatile int    xfer_running;
    int             xfer_thread_started;
    pthread_mutex_t xfer_mutex;
    xfer_ring_t     xfer_ring;

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
    /* Best-effort event journaling; downstream callbacks run regardless
     * so the event isn't lost even if persistence fails. */
    CUTILS_UNUSED(db_execute_non_query(mon->db,
        "INSERT INTO events (timestamp, severity, category, title, message) "
        "VALUES (?, ?, ?, ?, ?)",
        params, NULL));

    /* Fire callbacks */
    for (int i = 0; i < mon->nevent_cbs; i++) {
        if (mon->event_fns[i])
            mon->event_fns[i](severity, category, title, message,
                              mon->event_uds[i]);
    }
}

/* Monotonic millisecond timestamp for the xfer ring buffer. Monotonic
 * (not wall-clock) so NTP slew or DST changes don't make the ring's
 * timestamps go backwards mid-run. */
static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* Fast-poll thread body: reads register 2 every XFER_FAST_POLL_MS and
 * pushes any change into the ring. Only spawned when the driver supports
 * the single-register read (SRT/SMT do; HID drivers don't). */
static void *xfer_fast_poll_thread(void *arg)
{
    monitor_t *mon = arg;
    while (mon->xfer_running) {
        struct timespec sleep_ts = {
            .tv_sec  = 0,
            .tv_nsec = (long)XFER_FAST_POLL_MS * 1000000L,
        };
        nanosleep(&sleep_ts, NULL);
        if (!mon->xfer_running) break;

        uint16_t r;
        /* ups_read_transfer_reason takes ups->cmd_mutex internally, so
         * this serializes naturally with the slow poll, command writes,
         * and any API-driven config writes. */
        if (ups_read_transfer_reason(mon->ups, &r) != UPS_OK)
            continue;

        CUTILS_LOCK_GUARD(&mon->xfer_mutex);
        xfer_ring_push(&mon->xfer_ring, now_ms(), r);
    }
    return NULL;
}

/* Locked wrapper around xfer_ring_recent_cause — same semantics, plus
 * coordinates with the fast-poll thread's writes. */
static uint16_t recent_transfer_cause(monitor_t *mon, uint32_t lookback_ms)
{
    CUTILS_LOCK_GUARD(&mon->xfer_mutex);
    return xfer_ring_recent_cause(&mon->xfer_ring, now_ms(), lookback_ms);
}

/* Same as fire_event, but appends " (reason: <XferReason>)" to the message
 * when the fast-poll ring buffer has a recent non-AcceptableInput cause.
 * Drivers without the fast-poll path (HID) and events with no recent
 * cause both fall through to the plain fire_event. */
static void fire_event_xfer(monitor_t *mon, const char *severity,
                            const char *category, const char *title,
                            const char *message)
{
    uint16_t reason = recent_transfer_cause(mon, XFER_LOOKBACK_MS);
    if (!ups_transfer_reason_known(reason)) {
        fire_event(mon, severity, category, title, message);
        return;
    }
    char buf[640];
    snprintf(buf, sizeof(buf), "%s (reason: %s)",
             message, ups_transfer_reason_str(reason));
    fire_event(mon, severity, category, title, buf);
}

static void format_status_line(const ups_data_t *d, char *buf, size_t len)
{
    char status_str[256], eff_str[64];
    ups_status_str(d->status, status_str, sizeof(status_str));
    ups_efficiency_str((int)d->efficiency_reason, d->efficiency, eff_str, sizeof(eff_str));

    char freq_str[16];
    /* HID drivers leave output_frequency = NaN when the device doesn't
     * expose HID_USAGE_FREQUENCY; print a placeholder rather than "nanHz". */
    if (isnan(d->output_frequency))
        snprintf(freq_str, sizeof(freq_str), "—Hz");
    else
        snprintf(freq_str, sizeof(freq_str), "%.1fHz", d->output_frequency);

    snprintf(buf, len,
             "Status: %s | Chg: %.0f%% | Rt: %um%us | Bv: %.1fV | "
             "In: %.1fV | Out: %.1fV %s %.1fA | Load: %.0f%% | "
             "Eff: %s | Xfer: %s",
             status_str, d->charge_pct,
             d->runtime_sec / 60, d->runtime_sec % 60,
             d->battery_voltage, d->input_voltage,
             d->output_voltage, freq_str, d->output_current,
             d->load_pct, eff_str,
             ups_transfer_reason_str(d->transfer_reason));
}

/* --- Config register snapshot ---
 *
 * Diff-aware: only inserts a row when the live register value differs from
 * the most recent ups_config row for that register. First-ever read seeds
 * a 'baseline' row; subsequent deltas are 'external' (i.e. not written via
 * our API — usually a front-panel / LCD change). API writes stamp their
 * own inserts with source='api' so the diff sees them as the prior value
 * and doesn't re-record the same change as external. */

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
        /* Snapshot diff system is for tracking operator-relevant settings
         * (config category). MEASUREMENT/IDENTITY/DIAGNOSTIC values change
         * frequently or never and would either flood the ups_config table
         * or contribute nothing — skip them. */
        if (regs[i].category != UPS_REG_CATEGORY_CONFIG) continue;

        uint32_t raw = 0;
        if (ups_config_read(mon->ups, &regs[i], &raw, NULL, 0) != 0)
            continue;

        config_snapshot_decision_t decision =
            monitor_config_snapshot_decide(mon->db, regs[i].name, raw);
        if (decision == CONFIG_SNAPSHOT_SKIP) continue;
        const char *source = (decision == CONFIG_SNAPSHOT_BASELINE)
            ? "baseline" : "external";

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

        const char *ins_params[] = { regs[i].name, raw_s, display_s, ts, source, NULL };
        /* Best-effort per-register snapshot; snapped counter reflects
         * attempts, not successes — the summary log line is informational. */
        CUTILS_UNUSED(db_execute_non_query(mon->db,
            "INSERT INTO ups_config "
            "(register_name, raw_value, display_value, timestamp, source) "
            "VALUES (?, ?, ?, ?, ?)",
            ins_params, NULL));
        snapped++;
    }

    if (snapped > 0)
        log_info("config snapshot: %d change%s recorded", snapped,
                 snapped == 1 ? "" : "s");
}

/* --- Monitor thread --- */

static void *monitor_thread(void *arg)
{
    monitor_t *mon = arg;
    ups_data_t data;

    /* Use cached inventory from ups_connect() */
    if (mon->ups->has_inventory) {
        {
            CUTILS_LOCK_GUARD(&mon->mutex);
            mon->inventory = mon->ups->inventory;
        }
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
        {
            CUTILS_LOCK_GUARD(&mon->mutex);
            mon->data = data;
            mon->has_data = 1;
            mon->connected = 1;
            mon->prev_sig = status_signature(&data);
        }

        char line[512];
        format_status_line(&data, line, sizeof(line));
        log_info("%s", line);

        fire_event(mon, "info", "system", "Monitor Started", line);
        mon->was_connected = 1;
    }

    /* Poll loop */
    while (mon->running) {
        sleep((unsigned int)mon->poll_sec);
        if (!mon->running) break;

        /* Read UPS */
        memset(&data, 0, sizeof(data));
        if (ups_read_status(mon->ups, &data) != 0 ||
            ups_read_dynamic(mon->ups, &data) != 0) {
            /* Debounce connectivity events against the transport state,
             * not this single read. The recovery layer in ups.c keeps the
             * transport open across a handful of transient Modbus flakes
             * (CRC errors, inter-frame races on FTDI-backed links) and
             * only tears it down after MAX_CONSECUTIVE_ERRORS in a row.
             * Firing "UPS Disconnected" on every transient misread floods
             * the event log with phantom cycles even though comms never
             * actually dropped. */
            int transport_up = ups_is_connected(mon->ups);
            if (transport_up)
                log_warn("UPS read failed (transient — transport still open)");
            else
                log_error("UPS connection lost");

            {
                CUTILS_LOCK_GUARD(&mon->mutex);
                mon->connected = transport_up;
            }

            if (!transport_up && mon->was_connected) {
                mon->was_connected = 0;
                mon->disconnected_at = time(NULL);
                fire_event(mon, "error", "system", "UPS Disconnected",
                           "Lost communication with UPS");
            }
            continue;
        }

        {
            CUTILS_LOCK_GUARD(&mon->mutex);
            mon->data = data;
            mon->has_data = 1;
            mon->connected = 1;
        }

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
                    fire_event_xfer(mon, "info", "status",
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
                fire_event_xfer(mon, "warning", "power", "On Battery", msg);
            }
            if (cleared & UPS_ST_ON_BATTERY)
                fire_event_xfer(mon, "info", "power", "Utility Restored",
                                "UPS returned to utility power");
            if (set & UPS_ST_OUTPUT_OFF)
                fire_event_xfer(mon, "error", "power", "Output Off",
                                "UPS output has been turned off");
            if (cleared & UPS_ST_OUTPUT_OFF)
                fire_event_xfer(mon, "info", "power", "Output Restored",
                                "UPS output has been restored");

            /* Mode events */
            if (set & UPS_ST_HE_MODE)
                fire_event_xfer(mon, "info", "mode", "HE Mode Entered",
                                "UPS entered high efficiency mode");
            if (cleared & UPS_ST_HE_MODE)
                fire_event_xfer(mon, "info", "mode", "HE Mode Exited",
                                "UPS exited high efficiency mode");
            if (set & UPS_ST_BYPASS)
                fire_event_xfer(mon, "warning", "mode", "Bypass Entered",
                                (data.status & UPS_ST_COMMANDED)
                                    ? "UPS entered commanded bypass — output is unprotected"
                                    : "UPS transferred to bypass due to internal fault");
            if (cleared & UPS_ST_BYPASS)
                fire_event_xfer(mon, "info", "mode", "Bypass Exited",
                                "UPS returned to normal operation from bypass");

            /* Fault events */
            if (set & (UPS_ST_FAULT | UPS_ST_FAULT_STATE))
                fire_event_xfer(mon, "error", "fault", "Fault Detected",
                                "UPS has entered a fault condition");
            if (cleared & (UPS_ST_FAULT | UPS_ST_FAULT_STATE))
                fire_event_xfer(mon, "info", "fault", "Fault Cleared",
                                "UPS fault condition has been cleared");
            if (set & UPS_ST_OVERLOAD)
                fire_event_xfer(mon, "error", "fault", "Overload",
                                "UPS output is overloaded");
            if (cleared & UPS_ST_OVERLOAD)
                fire_event_xfer(mon, "info", "fault", "Overload Cleared",
                                "UPS overload condition has cleared");

            /* Test events */
            if (set & UPS_ST_TEST)
                fire_event_xfer(mon, "info", "test", "Self-Test Started",
                                "UPS self-test is in progress");
            if (cleared & UPS_ST_TEST)
                fire_event_xfer(mon, "info", "test", "Self-Test Ended",
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
                fire_event_xfer(mon, "critical", "shutdown",
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

        /* Daily config register snapshot (catches LCD-initiated changes) */
        time_t now = time(NULL);
        if (now - mon->last_config_snapshot >= 86400) {
            snapshot_config_registers(mon);
            mon->last_config_snapshot = now;
        }
    }

    return NULL;
}

/* --- Public API --- */

monitor_t *monitor_create(ups_t *ups, cutils_db_t *db,
                          int poll_interval_sec)
{
    monitor_t *mon = calloc(1, sizeof(*mon));
    if (!mon) return NULL;

    mon->ups = ups;
    mon->db = db;
    mon->poll_sec = poll_interval_sec > 0 ? poll_interval_sec : 2;
    mon->first_poll = 1;

    pthread_mutex_init(&mon->mutex, NULL);
    pthread_mutex_init(&mon->xfer_mutex, NULL);
    xfer_ring_init(&mon->xfer_ring);

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

    /* Spawn the transfer-reason fast-poll thread only when the active
     * driver supports the single-register read. HID drivers don't have
     * a separate cause register and leave read_transfer_reason NULL —
     * for those, we just rely on the slow status read's reason field
     * (which is also fine, since HID drivers don't latch a cause). */
    if (mon->ups->driver->read_transfer_reason) {
        mon->xfer_running = 1;
        rc = pthread_create(&mon->xfer_thread, NULL, xfer_fast_poll_thread, mon);
        if (rc != 0) {
            /* Non-fatal — slow poll still annotates events with whatever
             * register 2 holds at poll time. Log and continue. */
            mon->xfer_running = 0;
            log_warn("transfer-reason fast-poll thread failed to start "
                     "(events may show stale or missing reasons)");
        } else {
            mon->xfer_thread_started = 1;
            log_info("transfer-reason fast-poll started (%dms cadence, %dms lookback)",
                     XFER_FAST_POLL_MS, XFER_LOOKBACK_MS);
        }
    }

    log_info("monitor started (poll %ds)", mon->poll_sec);
    return CUTILS_OK;
}

void monitor_stop(monitor_t *mon)
{
    if (!mon) return;
    mon->running = 0;
    /* Stop the fast-poll thread first so it can't issue another bus read
     * after the main monitor loop has finished its final poll. Joining
     * before the main thread join is safe — the fast poller only takes
     * the cmd_mutex via ups_read_transfer_reason and doesn't touch any
     * monitor state the main thread owns. */
    if (mon->xfer_thread_started) {
        mon->xfer_running = 0;
        pthread_join(mon->xfer_thread, NULL);
        mon->xfer_thread_started = 0;
    }
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
    pthread_mutex_destroy(&mon->xfer_mutex);
    free(mon);
}

/* --- Thread-safe state accessors --- */

int monitor_get_status(monitor_t *mon, ups_data_t *out)
{
    CUTILS_LOCK_GUARD(&mon->mutex);
    if (!mon->has_data) return -1;
    *out = mon->data;
    return 0;
}

int monitor_get_inventory(monitor_t *mon, ups_inventory_t *out)
{
    CUTILS_LOCK_GUARD(&mon->mutex);
    *out = mon->inventory;
    return 0;
}

int monitor_get_snapshot(monitor_t *mon, status_snapshot_t *out)
{
    CUTILS_LOCK_GUARD(&mon->mutex);
    *out = mon->snapshot;
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
