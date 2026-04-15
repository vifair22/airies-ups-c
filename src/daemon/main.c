#include "app_config.h"

#include <cutils/log.h>
#include <cutils/appguard.h>
#include <cJSON.h>

#include "ups/ups.h"
#include "api/server.h"

#include <stdio.h>
#include <signal.h>
#include <unistd.h>

static volatile int running = 1;

static void sig_handler(int sig)
{
    (void)sig;
    running = 0;
}

/* --- Placeholder API route --- */

static api_response_t handle_status(const api_request_t *req, void *userdata)
{
    (void)req;
    (void)userdata;

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "status", "ok");
    cJSON_AddStringToObject(obj, "version", "0.1.0");
    cJSON_AddStringToObject(obj, "message", "airies-upsd running");

    return api_ok(cJSON_PrintUnformatted(obj));
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    /* Initialize c-utils (config, DB, migrations, logging, push) */
    appguard_config_t ag_cfg = app_appguard_config();
    appguard_t *guard = appguard_init(&ag_cfg);
    if (!guard)
        return 1;

    cutils_config_t *cfg = appguard_config(guard);

    /* Start API server */
    int http_port = config_get_int(cfg, "http.port", 8080);
    const char *socket_path = config_get_str(cfg, "http.socket");

    api_server_t *api = api_server_create(http_port, socket_path, NULL);
    if (!api) {
        log_error("failed to start API server");
        appguard_shutdown(guard);
        return 1;
    }

    /* Register routes */
    api_server_route(api, "/api/status", API_GET, handle_status, NULL);

    log_info("airies-upsd ready — http://localhost:%d", http_port);

    /* Main loop */
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    while (running)
        sleep(1);

    /* Cleanup */
    log_info("shutting down");
    api_server_stop(api);
    appguard_shutdown(guard);
    return 0;
}
