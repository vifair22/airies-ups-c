#ifndef API_ROUTES_H
#define API_ROUTES_H

#include "api/server.h"
#include "api/auth.h"
#include "monitor/monitor.h"
#include "shutdown/shutdown.h"
#include "weather/weather.h"
#include <cutils/db.h>
#include <cutils/config.h>
#include <cutils/appguard.h>

/* Context passed to all route handlers */
typedef struct {
    monitor_t       *monitor;
    ups_t           *ups;
    shutdown_mgr_t  *shutdown;
    weather_t       *weather;
    cutils_db_t     *db;
    cutils_config_t *config;
    appguard_t      *guard;
    /* Cached transfer voltage thresholds (read once, refreshed on config write) */
    uint16_t         transfer_high;
    uint16_t         transfer_low;
} route_ctx_t;

/* Register all API routes on the server. */
void api_register_routes(api_server_t *srv, route_ctx_t *ctx);

/* Re-read transfer voltage thresholds from UPS into cache.
 * Call after writing transfer config registers. */
void api_refresh_thresholds(route_ctx_t *ctx);

/* JSON response helper */
api_response_t api_ok_msg(const char *msg);

/* --- Sub-module route registration (called from api_register_routes) --- */
void api_register_auth_routes(api_server_t *srv, route_ctx_t *ctx);
void api_register_shutdown_routes(api_server_t *srv, route_ctx_t *ctx);
void api_register_config_routes(api_server_t *srv, route_ctx_t *ctx);
void api_register_weather_routes(api_server_t *srv, route_ctx_t *ctx);

#endif
