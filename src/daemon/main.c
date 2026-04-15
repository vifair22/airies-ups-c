#include "app_config.h"

#include <cutils/log.h>
#include <cutils/push.h>
#include <cutils/appguard.h>
#include <cutils/error_loop.h>
#include <cJSON.h>

#include "ups/ups.h"
#include "api/server.h"
#include "api/routes.h"
#include "monitor/monitor.h"
#include "alerts/alerts.h"
#include "shutdown/shutdown.h"

#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

static volatile int running = 1;

static void sig_handler(int sig)
{
    (void)sig;
    running = 0;
}

/* --- Event callback: push alerts to Pushover --- */

static void on_monitor_event(const char *severity, const char *category,
                             const char *title, const char *message,
                             void *userdata)
{
    (void)userdata;
    (void)category;

    /* Push warnings and above */
    if (strcmp(severity, "warning") == 0 ||
        strcmp(severity, "error") == 0 ||
        strcmp(severity, "critical") == 0) {
        push_send(title, message);
    }
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    /* --- Phase 1: AppGuard (config, DB, migrations, logging, push) --- */

    appguard_config_t ag_cfg = app_appguard_config();
    appguard_t *guard = appguard_init(&ag_cfg);
    if (!guard)
        return 1;

    cutils_config_t *cfg = appguard_config(guard);
    cutils_db_t *db = appguard_db(guard);

    /* --- Phase 2: UPS connection --- */

    const char *device = config_get_str(cfg, "ups.device");
    int baud = config_get_int(cfg, "ups.baud", 9600);
    int slave_id = config_get_int(cfg, "ups.slave_id", 1);

    log_info("connecting to UPS at %s (baud %d, slave %d)", device, baud, slave_id);
    ups_t *ups = ups_connect(device, baud, slave_id);
    if (!ups) {
        log_error("failed to connect to UPS at %s — running without UPS", device);
        /* Don't exit — the API server should still run for config/setup */
    } else {
        log_info("UPS connected — driver: %s", ups_driver_name(ups));
    }

    /* --- Phase 3: Subsystems --- */

    monitor_t *mon = NULL;
    shutdown_mgr_t *shutdown = NULL;
    alert_state_t alert_state;
    alert_config_t alert_cfg;
    alert_thresholds_t alert_thresh = { 0, 0 };

    if (ups) {
        /* Monitor */
        int poll_sec = config_get_int(cfg, "monitor.poll_interval", 2);
        int telem_sec = config_get_int(cfg, "monitor.telemetry_interval", 30);
        mon = monitor_create(ups, db, poll_sec, telem_sec);
        if (mon) {
            monitor_on_event(mon, on_monitor_event, NULL);
            monitor_start(mon);
        }

        /* Alerts */
        alerts_init(&alert_state);
        alert_cfg = alerts_load_config(cfg);
        ups_read_thresholds(ups, &alert_thresh.transfer_high,
                            &alert_thresh.transfer_low);
        if (alert_thresh.transfer_high > 0)
            log_info("transfer thresholds: high=%uV low=%uV",
                     alert_thresh.transfer_high, alert_thresh.transfer_low);

        /* TODO: wire alert_cfg + alert_state + alert_thresh into monitor
         * event loop for per-poll threshold checking */
        (void)alert_cfg;
        (void)alert_state;

        /* Shutdown orchestrator */
        shutdown = shutdown_create(db, ups);
    }

    /* --- Phase 4: API server --- */

    int http_port = config_get_int(cfg, "http.port", 8080);
    const char *socket_path = config_get_str(cfg, "http.socket");

    api_server_t *api = api_server_create(http_port, socket_path, NULL);
    if (!api) {
        log_error("failed to start API server");
        if (mon) monitor_stop(mon);
        if (ups) ups_close(ups);
        shutdown_free(shutdown);
        appguard_shutdown(guard);
        return 1;
    }

    /* Register routes */
    route_ctx_t route_ctx = {
        .monitor  = mon,
        .ups      = ups,
        .shutdown = shutdown,
        .db       = db,
        .config   = cfg,
    };
    api_register_routes(api, &route_ctx);

    log_info("airies-upsd ready — http://localhost:%d", http_port);

    /* --- Main loop --- */

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    while (running)
        sleep(1);

    /* --- Shutdown --- */

    log_info("shutting down");
    api_server_stop(api);
    if (mon) monitor_stop(mon);
    if (ups) ups_close(ups);
    shutdown_free(shutdown);
    appguard_shutdown(guard);

    return 0;
}
