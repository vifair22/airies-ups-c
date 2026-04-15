#ifndef MONITOR_H
#define MONITOR_H

#include "ups/ups.h"
#include <cutils/db.h>
#include <cutils/config.h>

/* --- UPS Monitor Subsystem ---
 *
 * Polls UPS status and dynamic blocks at a configurable interval.
 * Records telemetry to DB (downsampled).
 * Detects state changes and writes events to the journal.
 * Tracks HE inhibit state.
 * Exposes current UPS state for API queries. */

/* Monitor handle */
typedef struct monitor monitor_t;

/* Callback for state change events (alerts, shutdown trigger, etc.) */
typedef void (*monitor_event_fn)(const char *severity, const char *category,
                                 const char *title, const char *message,
                                 void *userdata);

/* Create and start the monitor.
 * ups must be connected.
 * poll_interval_sec: how often to read UPS (typically 2).
 * telemetry_interval_sec: how often to write to DB (typically 30).
 * Returns NULL on failure. */
monitor_t *monitor_create(ups_t *ups, cutils_db_t *db,
                          int poll_interval_sec, int telemetry_interval_sec);

/* Register an event callback (state changes, alerts, shutdown triggers).
 * Multiple callbacks supported (up to 4). */
int monitor_on_event(monitor_t *mon, monitor_event_fn fn, void *userdata);

/* Start the monitor loop (runs in a background thread). */
int monitor_start(monitor_t *mon);

/* Stop the monitor and free resources. */
void monitor_stop(monitor_t *mon);

/* --- Read current state (thread-safe) --- */

/* Get a snapshot of current UPS data. Returns 0 on success, -1 if no data yet. */
int monitor_get_status(monitor_t *mon, ups_data_t *out);

/* Get inventory (read once at startup). Returns 0 on success. */
int monitor_get_inventory(monitor_t *mon, ups_inventory_t *out);

/* Get the driver name. */
const char *monitor_driver_name(monitor_t *mon);

/* Is the UPS connected and responding? */
int monitor_is_connected(monitor_t *mon);

/* HE inhibit state */
int monitor_he_inhibit_active(monitor_t *mon);
const char *monitor_he_inhibit_source(monitor_t *mon);
void monitor_he_inhibit_set(monitor_t *mon, const char *source);
void monitor_he_inhibit_clear(monitor_t *mon);

#endif
