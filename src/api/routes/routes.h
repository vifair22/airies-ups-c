#ifndef API_ROUTES_H
#define API_ROUTES_H

#include "api/server.h"
#include "api/auth.h"
#include "api/sse.h"
#include "monitor/monitor.h"
#include "shutdown/shutdown.h"
#include "weather/weather.h"
#include <cutils/db.h>
#include <cutils/config.h>
#include <cutils/appguard.h>

/* Context passed to all route handlers */
typedef struct {
    monitor_t         *monitor;
    ups_t             *ups;
    shutdown_mgr_t    *shutdown;
    weather_t         *weather;
    sse_broadcaster_t *sse;
    cutils_db_t       *db;
    cutils_config_t   *config;
    appguard_t        *guard;
    /* Cached transfer voltage thresholds (read once, refreshed on config write) */
    uint16_t           transfer_high;
    uint16_t           transfer_low;
} route_ctx_t;

/* Register all API routes on the server. */
void api_register_routes(api_server_t *srv, route_ctx_t *ctx);

/* Re-read transfer voltage thresholds from UPS into cache.
 * Call after writing transfer config registers. */
void api_refresh_thresholds(route_ctx_t *ctx);

/* monitor_poll_fn-shaped state-tick callback. Reuses the same `/api/ups/state`
 * JSON shape as the buffered route. userdata is route_ctx_t* — the broadcaster
 * is read from ctx->sse and may be NULL (in which case this is a no-op). */
void api_state_emit(const ups_data_t *data, void *userdata);

/* JSON response helper */
api_response_t api_ok_msg(const char *msg);

/* --- Sub-module route registration (called from api_register_routes) --- */
void api_register_auth_routes(api_server_t *srv, route_ctx_t *ctx);
void api_register_shutdown_routes(api_server_t *srv, route_ctx_t *ctx);
void api_register_config_routes(api_server_t *srv, route_ctx_t *ctx);
void api_register_weather_routes(api_server_t *srv, route_ctx_t *ctx);

/* --- Auth route handlers (exposed for direct unit testing) ---
 *
 * These are normally registered via api_register_auth_routes and called
 * by libmicrohttpd through the request_handler dispatcher. The
 * signatures match api_handler_fn so a test can construct an
 * api_request_t + route_ctx_t and call them directly without an MHD
 * instance — useful for asserting on response shape, status code, and
 * Set-Cookie attribute strings. */
api_response_t handle_auth_setup (const api_request_t *req, void *ud);
api_response_t handle_auth_login (const api_request_t *req, void *ud);
api_response_t handle_auth_change(const api_request_t *req, void *ud);
api_response_t handle_auth_logout(const api_request_t *req, void *ud);
api_response_t handle_auth_check (const api_request_t *req, void *ud);
api_response_t handle_setup_status(const api_request_t *req, void *ud);
api_response_t handle_setup_ports (const api_request_t *req, void *ud);
api_response_t handle_setup_test  (const api_request_t *req, void *ud);

#endif
