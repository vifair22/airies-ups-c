#include "monitor/fast_loop.h"
#include "monitor/xfer_ring.h"
#include "ups/ups_format.h"

#include <cutils/log.h>
#include <cutils/mem.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FAST_POLL_MS    200
#define LOOKBACK_MS     5000

/* --- State --- */

struct fast_loop {
    ups_t           *ups;
    cutils_db_t     *db;
    monitor_t       *mon;

    pthread_t        thread;
    volatile int     running;
    int              thread_started;

    pthread_mutex_t  ring_mutex;
    xfer_ring_t      ring;

    /* Power-state bits this loop owns. The slow loop's per-bit
     * detection skips these so we don't double-fire. */
    uint32_t         prev_status_fast;
    int              first_tick;
};

/* Subset of UPS_ST_* bits emitted by the fast loop. The slow loop
 * still passes data.status through as-is — this mask only governs
 * which transitions the fast loop fires events for. */
#define FAST_OWNED_BITS (UPS_ST_ON_BATTERY | UPS_ST_OUTPUT_OFF | \
                         UPS_ST_BYPASS     | UPS_ST_FAULT      | \
                         UPS_ST_FAULT_STATE| UPS_ST_OVERLOAD)

/* --- Time helpers --- */

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* "YYYY-MM-DD HH:MM:SS.mmm" UTC — matches the cutils logs convention
 * and pairs well with the events table for cross-table correlation.
 * Explicit % 1000 + int cast keeps the millisecond field bounded to
 * 3 digits so -Wformat-truncation can prove the buffer is sized
 * correctly under -O2 (without it, gcc assumes the long can be any
 * size and warns). */
static void format_utc_ms(char *buf, size_t len)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm;
    gmtime_r(&ts.tv_sec, &tm);
    char base[24];
    strftime(base, sizeof(base), "%Y-%m-%d %H:%M:%S", &tm);
    int ms = (int)((ts.tv_nsec / 1000000L) % 1000);
    snprintf(buf, len, "%s.%03d", base, ms);
}

/* --- Persistence --- */

/* Best-effort INSERT into xfer_history. status_bits is captured from
 * the same tick as the reason so post-mortem queries can distinguish
 * "register 2 said LowBatteryVoltage while UPS stayed Online" (firmware
 * noise / AVR ridethrough) from "register 2 said LowBatteryVoltage
 * while UPS_ST_ON_BATTERY was set" (real transient transfer). */
static void persist_transition(fast_loop_t *fl, uint16_t reason, uint32_t status_bits)
{
    char ts[32];
    format_utc_ms(ts, sizeof(ts));

    char code_s[8];
    char status_s[16];
    snprintf(code_s,   sizeof(code_s),   "%u", reason);
    snprintf(status_s, sizeof(status_s), "%u", status_bits);

    const char *params[] = {
        ts, code_s, ups_transfer_reason_str(reason), status_s, NULL
    };
    CUTILS_UNUSED(db_execute_non_query(fl->db,
        "INSERT INTO xfer_history (timestamp, reason_code, reason_str, status_bits) "
        "VALUES (?, ?, ?, ?)",
        params, NULL));
}

/* --- Event detection --- */

/* Detect transitions in the fast-owned bit subset against
 * prev_status_fast and emit one event per changed bit. Reason is the
 * register-2 value just read at the same tick; passed through to
 * monitor_fire_event_with_reason which appends "(reason: ...)" when
 * the value is known and non-AcceptableInput. */
static void emit_transitions(fast_loop_t *fl, uint32_t status, uint16_t reason)
{
    uint32_t changed = (fl->prev_status_fast ^ status) & FAST_OWNED_BITS;
    if (!changed) return;

    uint32_t set     = changed & status;
    uint32_t cleared = changed & fl->prev_status_fast;

    char msg[256];

    /* Power events */
    if (set & UPS_ST_ON_BATTERY) {
        ups_data_t data;
        if (ups_read_status(fl->ups, &data) == 0) {
            snprintf(msg, sizeof(msg),
                     "UPS switched to battery — charge %.0f%%, runtime %um%us",
                     data.charge_pct, data.runtime_sec / 60,
                     data.runtime_sec % 60);
        } else {
            snprintf(msg, sizeof(msg), "UPS switched to battery");
        }
        monitor_fire_event_with_reason(fl->mon, "warning", "power",
                                       "On Battery", msg, reason);
    }
    if (cleared & UPS_ST_ON_BATTERY)
        monitor_fire_event_with_reason(fl->mon, "info", "power",
                                       "Utility Restored",
                                       "UPS returned to utility power", reason);
    if (set & UPS_ST_OUTPUT_OFF)
        monitor_fire_event_with_reason(fl->mon, "error", "power", "Output Off",
                                       "UPS output has been turned off", reason);
    if (cleared & UPS_ST_OUTPUT_OFF)
        monitor_fire_event_with_reason(fl->mon, "info", "power", "Output Restored",
                                       "UPS output has been restored", reason);

    /* Bypass — message varies by commanded vs fault-driven. We can't
     * reach the COMMANDED bit from the fast loop without another read,
     * so fall back to a neutral message; the slow loop's HE/error
     * events provide context if a fault actually drove the bypass. */
    if (set & UPS_ST_BYPASS)
        monitor_fire_event_with_reason(fl->mon, "warning", "mode", "Bypass Entered",
                                       "UPS transferred to bypass", reason);
    if (cleared & UPS_ST_BYPASS)
        monitor_fire_event_with_reason(fl->mon, "info", "mode", "Bypass Exited",
                                       "UPS returned to normal operation from bypass",
                                       reason);

    /* Fault — coalesced (slow loop did the same). */
    if (set & (UPS_ST_FAULT | UPS_ST_FAULT_STATE))
        monitor_fire_event_with_reason(fl->mon, "error", "fault", "Fault Detected",
                                       "UPS has entered a fault condition", reason);
    if (cleared & (UPS_ST_FAULT | UPS_ST_FAULT_STATE))
        monitor_fire_event_with_reason(fl->mon, "info", "fault", "Fault Cleared",
                                       "UPS fault condition has been cleared", reason);

    if (set & UPS_ST_OVERLOAD)
        monitor_fire_event_with_reason(fl->mon, "error", "fault", "Overload",
                                       "UPS output is overloaded", reason);
    if (cleared & UPS_ST_OVERLOAD)
        monitor_fire_event_with_reason(fl->mon, "info", "fault", "Overload Cleared",
                                       "UPS overload condition has cleared", reason);
}

/* --- Thread body --- */

static void *fast_loop_thread(void *arg)
{
    fast_loop_t *fl = arg;

    while (fl->running) {
        struct timespec sleep_ts = {
            .tv_sec  = 0,
            .tv_nsec = (long)FAST_POLL_MS * 1000000L,
        };
        nanosleep(&sleep_ts, NULL);
        if (!fl->running) break;

        /* Read both registers we care about. ups_read_status takes
         * cmd_mutex internally; ups_read_transfer_reason same. The
         * paced Modbus wrapper ensures the two calls are at least
         * UPS_MB_MIN_GAP_MS apart even though the calls themselves
         * are back-to-back. */
        ups_data_t data;
        if (ups_read_status(fl->ups, &data) != UPS_OK)
            continue;

        uint16_t reason;
        if (ups_read_transfer_reason(fl->ups, &reason) != UPS_OK)
            continue;

        /* Ring + persistence on register-2 transitions. The ring's
         * de-dupe means we only persist genuine changes — at steady
         * state with reason == AcceptableInput we generate no DB
         * traffic. */
        int recorded;
        {
            CUTILS_LOCK_GUARD(&fl->ring_mutex);
            recorded = xfer_ring_push(&fl->ring, monotonic_ms(), reason);
        }
        if (recorded)
            persist_transition(fl, reason, data.status);

        /* Power-state bit transitions. Skip on the very first tick so
         * we don't fire spurious events against a zero baseline — the
         * slow loop's snapshot reload handles the daemon-restart case
         * for the bits it owns; for the fast-owned bits, treating the
         * first observed status as the baseline is the right call (a
         * fault that survives a daemon restart will already be visible
         * via the slow loop's snapshot-driven detection). */
        if (!fl->first_tick)
            emit_transitions(fl, data.status, reason);
        fl->prev_status_fast = data.status;
        fl->first_tick = 0;
    }
    return NULL;
}

/* --- Public API --- */

fast_loop_t *fast_loop_start(ups_t *ups, cutils_db_t *db, monitor_t *mon)
{
    if (!ups || !ups->driver->read_transfer_reason) return NULL;

    fast_loop_t *fl = calloc(1, sizeof(*fl));
    if (!fl) return NULL;

    fl->ups        = ups;
    fl->db         = db;
    fl->mon        = mon;
    fl->first_tick = 1;
    pthread_mutex_init(&fl->ring_mutex, NULL);
    xfer_ring_init(&fl->ring);

    fl->running = 1;
    if (pthread_create(&fl->thread, NULL, fast_loop_thread, fl) != 0) {
        fl->running = 0;
        pthread_mutex_destroy(&fl->ring_mutex);
        free(fl);
        return NULL;
    }
    fl->thread_started = 1;
    log_info("fast loop started (%dms cadence, %dms lookback)",
             FAST_POLL_MS, LOOKBACK_MS);
    return fl;
}

void fast_loop_stop(fast_loop_t *fl)
{
    if (!fl) return;
    if (fl->thread_started) {
        fl->running = 0;
        pthread_join(fl->thread, NULL);
    }
    pthread_mutex_destroy(&fl->ring_mutex);
    free(fl);
}

uint16_t fast_loop_recent_cause(fast_loop_t *fl, uint32_t lookback_ms)
{
    if (!fl) return UPS_TRANSFER_REASON_UNKNOWN;
    CUTILS_LOCK_GUARD(&fl->ring_mutex);
    return xfer_ring_recent_cause(&fl->ring, monotonic_ms(), lookback_ms);
}
