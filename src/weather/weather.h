#ifndef WEATHER_H
#define WEATHER_H

#include "monitor/monitor.h"
#include <cutils/db.h>

/* --- Weather Subsystem ---
 *
 * Polls NWS alerts and hourly forecast for severe weather.
 * When severe conditions detected, inhibits HE mode via narrow
 * frequency tolerance. Restores when conditions clear.
 *
 * Runs in a background thread with configurable poll interval.
 * Config stored in weather_config DB table. */

typedef struct weather weather_t;

/* Create the weather subsystem.
 * monitor is used for HE inhibit control.
 * ups is used for frequency tolerance commands.
 * Returns NULL if weather is disabled in config. */
weather_t *weather_create(cutils_db_t *db, monitor_t *monitor, ups_t *ups);

/* Start polling (background thread). */
int weather_start(weather_t *w);

/* Stop and free. */
void weather_stop(weather_t *w);

/* Get current weather assessment as JSON (malloc'd, caller frees). */
char *weather_status_json(weather_t *w);

/* Simulate a severe weather event. Injected into the next poll cycle
 * as if NWS reported it. One-shot: consumed after one cycle. */
void weather_simulate_severe(weather_t *w, const char *reason);

/* Clear a simulated event and force restore on next cycle. */
void weather_simulate_clear(weather_t *w);

/* Fetch current NWS data and return as JSON (malloc'd, caller frees).
 * Includes active alerts and forecast periods for display. */
char *weather_report_json(weather_t *w);

#endif
