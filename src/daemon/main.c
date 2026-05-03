#include "config/app_config.h"

#include <cutils/log.h>
#include <cutils/push.h>
#include <cutils/appguard.h>
#include <cutils/error_loop.h>
#include <cutils/version.h>

#include "ups/ups.h"
#include "api/server.h"
#include "api/sse.h"
#include "api/routes/routes.h"
#include "api/auth.h"
#include "monitor/monitor.h"
#include "alerts/alerts.h"
#include "shutdown/shutdown.h"
#include "weather/weather.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

static volatile int running = 1;
static volatile int restart_requested = 0;
static const char *g_ups_name = NULL;

/* Set when the monitor is created so alert_notify can route alerts
 * through the events journal. NULL means alerts log + push but don't
 * journal (e.g. before the monitor is up — shouldn't happen in
 * practice since the alert engine is wired only after monitor_create
 * succeeds). */
static monitor_t *g_monitor = NULL;

static void sig_handler(int sig)
{
    (void)sig;
    running = 0;
}

/* Called from API restart endpoint — sets flag for main loop.
 * Declared extern in routes.c. */
void app_request_restart(void);
void app_request_restart(void)
{
    restart_requested = 1;
    running = 0;
}

/* --- Push notification formatting --- */

typedef struct {
    const char *symbol;
    const char *label;
    const char *color;
    int         priority;
} sev_format_t;

static sev_format_t sev_format(const char *severity)
{
    if (strcmp(severity, "critical") == 0)
        return (sev_format_t){
            .symbol = "\xe2\x9a\xa0", .label = "CRITICAL",
            .color = "#ef4444", .priority = PUSH_PRIORITY_HIGH };
    if (strcmp(severity, "error") == 0)
        return (sev_format_t){
            .symbol = "\xe2\x9a\xa0", .label = "ERROR",
            .color = "#ef4444", .priority = PUSH_PRIORITY_HIGH };
    if (strcmp(severity, "warning") == 0)
        return (sev_format_t){
            .symbol = "\xe2\x9a\xa0", .label = "WARNING",
            .color = "#eab308", .priority = PUSH_PRIORITY_NORMAL };
    /* info / default */
    return (sev_format_t){
        .symbol = "\xe2\x9c\x93", .label = "INFO",
        .color = "#60a5fa", .priority = PUSH_PRIORITY_NORMAL };
}

static void push_notify(const char *severity, const char *title,
                        const char *body)
{
    sev_format_t fmt = sev_format(severity);
    char t[256], b[1024];

    if (g_ups_name && g_ups_name[0])
        snprintf(t, sizeof(t), "%s %s: %s", fmt.symbol, g_ups_name, title);
    else
        snprintf(t, sizeof(t), "%s %s", fmt.symbol, title);

    if (g_ups_name && g_ups_name[0])
        snprintf(b, sizeof(b),
                 "<font color=\"%s\"><b>%s</b></font> \xe2\x80\x94 %s\n%s",
                 fmt.color, fmt.label, g_ups_name, body);
    else
        snprintf(b, sizeof(b),
                 "<font color=\"%s\"><b>%s</b></font>\n%s",
                 fmt.color, fmt.label, body);

    push_opts_t opts = {
        .title    = t,
        .message  = b,
        .html     = 1,
        .priority = fmt.priority,
    };
    /* Best-effort notification; a failure here must not cascade into the
     * event pipeline that triggered it. */
    CUTILS_UNUSED(push_send_opts(&opts));
}

/* --- Event callback: push alerts to Pushover --- */

static int severity_rank(const char *s)
{
    if (strcmp(s, "critical") == 0) return 4;
    if (strcmp(s, "error") == 0)    return 3;
    if (strcmp(s, "warning") == 0)  return 2;
    if (strcmp(s, "info") == 0)     return 1;
    return 0;
}

static void on_monitor_event(const char *severity, const char *category,
                             const char *title, const char *message,
                             void *userdata)
{
    cutils_config_t *cfg = userdata;
    (void)category;

    const char *min = config_get_str(cfg, "push.min_severity");
    if (!min) min = "warning";
    if (strcmp(min, "off") == 0) return;

    if (severity_rank(severity) >= severity_rank(min))
        push_notify(severity, title, message);
}

/* --- Alert integration --- */

typedef struct {
    alert_state_t      state;
    alert_config_t     cfg;
    alert_thresholds_t thresh;
    shutdown_mgr_t    *shutdown;
} alert_ctx_t;

static void alert_notify(const char *severity, const char *category,
                         const char *title, const char *body)
{
    log_info("ALERT: %s — %s", title, body);
    /* Route through the monitor's event journal so threshold transitions
     * land in the events table alongside bit-transition events. The
     * journal write also fires on_monitor_event, which handles the push
     * with the correct severity gating — so we don't push directly here.
     * Falls back to a direct push if the monitor isn't up yet. */
    if (g_monitor)
        monitor_fire_event(g_monitor, severity, category, title, body);
    else
        push_notify(severity, title, body);
}

static void on_monitor_poll(const ups_data_t *data, void *userdata)
{
    alert_ctx_t *actx = userdata;
    alerts_check(&actx->state, data, &actx->thresh, &actx->cfg, alert_notify);
    if (actx->shutdown)
        shutdown_check_trigger(actx->shutdown, data);
}

int main(int argc, char *argv[])
{
    /* Line-buffer stdout/stderr so log lines flush on every '\n'. Without
     * this, libc block-buffers stdout when journald hands us a pipe, which
     * lets the parent's partial buffer interleave with a forked child's
     * stdout — journald then flushes the partial bytes as a binary blob
     * with `_LINE_BREAK=pid-change`. Must run before any I/O. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IOLBF, 0);

    appguard_set_argv(argc, argv);

    /* --- Phase 1: AppGuard (config, DB, migrations, logging, push) --- */

    appguard_config_t ag_cfg = app_appguard_config();
    appguard_t *guard = appguard_init(&ag_cfg);
    if (!guard)
        return 1;

    /* Field-diagnostic breadcrumb: pin the c-utils version stamp into
     * the journal at startup. Lets us tell at a glance which lib build
     * a misbehaving Pi is actually running without SSH'ing in to grep
     * the binary. Daemon's own VERSION_STRING is already journaled by
     * appguard_init's banner. */
    log_info("c-utils %s", cutils_version());

    cutils_config_t *cfg = appguard_config(guard);
    cutils_db_t *db = appguard_db(guard);

    const char *name = config_get_str(cfg, "setup.ups_name");
    g_ups_name = (name && name[0]) ? strdup(name) : NULL;

    /* --- Phase 2: UPS connection --- */

    int ups_configured = config_get_int(cfg, "setup.ups_done", 0);
    ups_t *ups = NULL;

    if (!ups_configured) {
        log_info("UPS not configured — skipping connection (setup required)");
    } else {
        const char *conn_type = config_get_str(cfg, "ups.conn_type");
        if (!conn_type) conn_type = "serial";

        ups_conn_params_t conn_params = {0};

        if (strcmp(conn_type, "usb") == 0) {
            const char *vid_str = config_get_str(cfg, "ups.usb_vid");
            const char *pid_str = config_get_str(cfg, "ups.usb_pid");
            conn_params.type = UPS_CONN_USB;
            conn_params.usb.vendor_id = (uint16_t)strtol(vid_str ? vid_str : "051d", NULL, 16);
            conn_params.usb.product_id = (uint16_t)strtol(pid_str ? pid_str : "0002", NULL, 16);
            conn_params.usb.serial = NULL;
            log_info("connecting to UPS via USB (VID=%04x PID=%04x)",
                     conn_params.usb.vendor_id, conn_params.usb.product_id);
        } else {
            const char *device = config_get_str(cfg, "ups.device");
            int baud = config_get_int(cfg, "ups.baud", 9600);
            int slave_id = config_get_int(cfg, "ups.slave_id", 1);
            conn_params.type = UPS_CONN_SERIAL;
            conn_params.serial.device = device;
            conn_params.serial.baud = baud;
            conn_params.serial.slave_id = slave_id;
            log_info("connecting to UPS at %s (baud %d, slave %d)", device, baud, slave_id);
        }

        int rc = ups_connect(&conn_params, &ups);
        while (rc != UPS_OK && running) {
            const char *why = (rc == UPS_ERR_NO_DRIVER)
                              ? "no matching driver identified the UPS"
                              : "transport connect failed";
            log_warn("UPS not found (%s) — retrying in 5s", why);
            sleep(5);
            if (!running) break;
            rc = ups_connect(&conn_params, &ups);
        }
        if (ups)
            log_info("UPS connected — driver: %s", ups_driver_name(ups));
    }

    /* --- Phase 3: Subsystems --- */

    monitor_t *mon = NULL;
    shutdown_mgr_t *shutdown = NULL;
    sse_broadcaster_t *sse = NULL;
    weather_t *weather = NULL;
    /* route_ctx is built incrementally so monitor_on_poll can register the
     * SSE state-tick callback against a stable pointer BEFORE monitor_start.
     * Fields not yet available (weather) are filled in later — the state-emit
     * callback only reads monitor/ups/sse/config/transfer_*, never weather. */
    static route_ctx_t route_ctx;
    static alert_ctx_t alert_ctx;

    if (ups) {
        /* Monitor */
        int poll_sec = config_get_int(cfg, "monitor.poll_interval", 5);
        mon = monitor_create(ups, db, poll_sec);
        if (mon) {
            g_monitor = mon;
            monitor_on_event(mon, on_monitor_event, cfg);

            /* SSE broadcaster: monitor events fan out to web-UI subscribers
             * via /api/events/stream. State ticks (full ups_data snapshot)
             * are emitted from the slow-loop poll callback wired below. */
            sse = sse_broadcaster_create(0);
            if (sse)
                monitor_on_event(mon, sse_on_monitor_event, sse);

            /* Wire alert engine into monitor poll cycle. Seed alert state
             * from the persisted snapshot so we don't re-fire alerts on
             * restart for conditions that were already active when the
             * daemon last ran (e.g. an unresolved fault). monitor_start
             * has already loaded the snapshot during its thread bring-up. */
            alerts_init(&alert_ctx.state);
            {
                status_snapshot_t snap;
                if (monitor_get_snapshot(mon, &snap) == 0)
                    alerts_seed_from_snapshot(&alert_ctx.state, &snap);
            }
            alert_ctx.cfg = alerts_load_config(cfg);
            ups_read_thresholds(ups, &alert_ctx.thresh.transfer_high,
                                &alert_ctx.thresh.transfer_low);
            if (alert_ctx.thresh.transfer_high > 0)
                log_info("transfer thresholds: high=%uV low=%uV",
                         alert_ctx.thresh.transfer_high,
                         alert_ctx.thresh.transfer_low);
            /* Shutdown orchestrator (created before monitor starts so
             * the poll callback can evaluate trigger conditions) */
            shutdown = shutdown_create(db, ups, cfg, mon);
            alert_ctx.shutdown = shutdown;

            /* Populate route_ctx with everything we have so far. weather is
             * filled in below, after weather_create. State-emit only reads
             * monitor/ups/sse/config/transfer_*, so partial init is safe. */
            route_ctx = (route_ctx_t){
                .monitor  = mon,
                .ups      = ups,
                .shutdown = shutdown,
                .sse      = sse,
                .db       = db,
                .config   = cfg,
                .guard    = guard,
            };
            api_refresh_thresholds(&route_ctx);

            monitor_on_poll(mon, on_monitor_poll, &alert_ctx);
            /* SSE state ticks: emit a full ups_data snapshot every slow-loop
             * cycle so dashboards can drop their /api/ups/state polling. */
            monitor_on_poll(mon, api_state_emit, &route_ctx);

            monitor_start(mon);
        }
    }

    /* Weather subsystem (starts only if enabled in DB config) */
    if (ups && mon)
        weather = weather_create(db, mon, ups);
    if (weather)
        weather_start(weather);
    route_ctx.weather = weather;

    /* --- Phase 4: API server --- */

    int http_port = config_get_int(cfg, "http.port", 8080);
    const char *socket_path = config_get_str(cfg, "http.socket");

    api_server_t *api = api_server_create(http_port, socket_path, "frontend/dist");
    if (!api) {
        log_error("failed to start API server");
        if (mon) monitor_stop(mon);
        if (ups) ups_close(ups);
        shutdown_free(shutdown);
        appguard_shutdown(guard);
        return 1;
    }

    api_register_routes(api, &route_ctx);
    api_server_set_auth(api, auth_check, db);

    log_info("airies-upsd ready — http://localhost:%d", http_port);

    /* --- Main loop --- */

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    while (running)
        sleep(1);

    /* --- Shutdown --- */

    log_info("shutting down");
    weather_stop(weather);
    /* Wake any blocked SSE readers BEFORE stopping the API server so
     * MHD's worker threads exit promptly instead of hanging on cond_wait. */
    sse_broadcaster_shutdown(sse);
    api_server_stop(api);
    if (mon) monitor_stop(mon);
    if (ups) ups_close(ups);
    shutdown_free(shutdown);
    /* Broadcaster destroyed last — MHD has already drained subscribers
     * (each cleanup callback freed its own subscriber slot). */
    sse_broadcaster_destroy(sse);

    if (restart_requested) {
        log_info("restarting via appguard_restart");
        /* appguard_restart shuts the guard down internally on every
         * return path (success exec's, failure paths free first). The
         * guard pointer is invalid after this call regardless of return
         * value, so we must not call appguard_shutdown again below. */
        appguard_restart(guard);
        fprintf(stderr, "airies-ups: restart failed, exiting\n");
        return 1;
    }

    appguard_shutdown(guard);

    return 0;
}
