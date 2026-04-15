#ifndef API_ROUTES_H
#define API_ROUTES_H

#include "api/server.h"
#include "api/auth.h"
#include "monitor/monitor.h"
#include "shutdown/shutdown.h"
#include <cutils/db.h>
#include <cutils/config.h>

/* Context passed to all route handlers */
typedef struct {
    monitor_t       *monitor;
    ups_t           *ups;
    shutdown_mgr_t  *shutdown;
    cutils_db_t     *db;
    cutils_config_t *config;
} route_ctx_t;

/* Register all API routes on the server. */
void api_register_routes(api_server_t *srv, route_ctx_t *ctx);

#endif
