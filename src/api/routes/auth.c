#include "api/routes/routes.h"
#include <cJSON.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>

/* --- Helpers --- */

static void auth_event(cutils_db_t *db, const char *severity,
                       const char *title, const char *message)
{
    char ts[32];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

    const char *params[] = { ts, severity, "auth", title, message, NULL };
    db_execute_non_query(db,
        "INSERT INTO events (timestamp, severity, category, title, message) "
        "VALUES (?, ?, ?, ?, ?)",
        params, NULL);
}

/* --- Auth endpoints --- */

static api_response_t handle_auth_setup(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;
    if (auth_is_setup(ctx->db))
        return api_error(400, "admin password already set");

    if (!req->body) return api_error(400, "request body required");
    cJSON *body = cJSON_Parse(req->body);
    if (!body) return api_error(400, "invalid JSON");

    const cJSON *jpw = cJSON_GetObjectItem(body, "password");
    if (!jpw || !cJSON_IsString(jpw) || strlen(jpw->valuestring) < 4) {
        cJSON_Delete(body);
        return api_error(400, "password must be at least 4 characters");
    }

    auth_set_password(ctx->db, jpw->valuestring);
    cJSON_Delete(body);
    auth_event(ctx->db, "info", "Password Set", "Admin password configured during initial setup");

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "password set");
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return api_ok(json);
}

static api_response_t handle_auth_login(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;
    if (!req->body) return api_error(400, "request body required");
    cJSON *body = cJSON_Parse(req->body);
    if (!body) return api_error(400, "invalid JSON");

    const cJSON *jpw = cJSON_GetObjectItem(body, "password");
    if (!jpw || !cJSON_IsString(jpw)) {
        cJSON_Delete(body);
        return api_error(400, "missing 'password'");
    }

    if (!auth_verify_password(ctx->db, jpw->valuestring)) {
        cJSON_Delete(body);
        auth_event(ctx->db, "warning", "Login Failed", "Invalid password attempt");
        return api_error(401, "invalid password");
    }
    cJSON_Delete(body);
    auth_event(ctx->db, "info", "Login", "Admin login successful");

    char *token = auth_create_token(ctx->db, 24 * 365);
    if (!token) return api_error(500, "failed to create session");

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "token", token);
    cJSON_AddNumberToObject(resp, "expires_in", 24 * 365 * 3600);
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    free(token);
    return api_ok(json);
}

static api_response_t handle_auth_change(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;
    if (!req->body) return api_error(400, "request body required");
    cJSON *body = cJSON_Parse(req->body);
    if (!body) return api_error(400, "invalid JSON");

    const cJSON *jold = cJSON_GetObjectItem(body, "old_password");
    const cJSON *jnew = cJSON_GetObjectItem(body, "new_password");
    if (!jold || !cJSON_IsString(jold) || !jnew || !cJSON_IsString(jnew)) {
        cJSON_Delete(body);
        return api_error(400, "missing 'old_password' and 'new_password'");
    }
    if (strlen(jnew->valuestring) < 4) {
        cJSON_Delete(body);
        return api_error(400, "new password must be at least 4 characters");
    }
    if (!auth_verify_password(ctx->db, jold->valuestring)) {
        cJSON_Delete(body);
        return api_error(401, "current password is incorrect");
    }

    int rc = auth_set_password(ctx->db, jnew->valuestring);
    cJSON_Delete(body);

    if (rc != 0) return api_error(500, "failed to update password");

    auth_event(ctx->db, "info", "Password Changed", "Admin password updated");

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "password changed");
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return api_ok(json);
}

static api_response_t handle_auth_logout(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;

    /* Delete the session token from DB */
    if (req->auth_token) {
        const char *tok = req->auth_token;
        if (strncmp(tok, "Bearer ", 7) == 0) tok += 7;
        const char *params[] = { tok, NULL };
        db_execute_non_query(ctx->db,
            "DELETE FROM sessions WHERE token = ?", params, NULL);
    }

    auth_event(ctx->db, "info", "Logout", "Admin session ended");

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "logged out");
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return api_ok(json);
}

/* --- Setup endpoints --- */

static api_response_t handle_setup_status(const api_request_t *req, void *ud)
{
    (void)req;
    route_ctx_t *ctx = ud;

    int password_set  = auth_is_setup(ctx->db);
    int ups_configured = config_get_int(ctx->config, "setup.ups_done", 0) != 0;

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "needs_setup", !password_set || !ups_configured);
    cJSON_AddBoolToObject(obj, "password_set", password_set != 0);
    cJSON_AddBoolToObject(obj, "ups_configured", ups_configured);
    cJSON_AddBoolToObject(obj, "ups_connected", ctx->ups && monitor_is_connected(ctx->monitor));
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return api_ok(json);
}

static api_response_t handle_setup_ports(const api_request_t *req, void *ud)
{
    (void)req;
    (void)ud;

    cJSON *obj = cJSON_CreateObject();

    /* Scan for serial devices */
    cJSON *serial = cJSON_CreateArray();
    const char *patterns[] = { "/dev/ttyUSB", "/dev/ttyACM", "/dev/ttyS", NULL };
    for (int p = 0; patterns[p]; p++) {
        for (int i = 0; i < 10; i++) {
            char path[64];
            snprintf(path, sizeof(path), "%s%d", patterns[p], i);
            if (access(path, R_OK | W_OK) == 0)
                cJSON_AddItemToArray(serial, cJSON_CreateString(path));
        }
    }
    cJSON_AddItemToObject(obj, "serial", serial);

    /* Scan for USB HID power devices */
    cJSON *usb = cJSON_CreateArray();
    DIR *dir = opendir("/sys/class/hidraw");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char uevent[512];
            snprintf(uevent, sizeof(uevent),
                     "/sys/class/hidraw/%s/device/uevent", ent->d_name);
            FILE *f = fopen(uevent, "r");
            if (!f) continue;

            char line[256];
            unsigned int vid = 0, pid = 0;
            char hid_name[128] = "";
            while (fgets(line, sizeof(line), f)) {
                unsigned int bus;
                if (sscanf(line, "HID_ID=%x:%x:%x", &bus, &vid, &pid) == 3)
                    continue;
                if (sscanf(line, "HID_NAME=%127[^\n]", hid_name) == 1)
                    continue;
            }
            fclose(f);

            if (vid > 0) {
                cJSON *dev = cJSON_CreateObject();
                char vid_s[8], pid_s[8], devpath[512];
                snprintf(vid_s, sizeof(vid_s), "%04x", vid);
                snprintf(pid_s, sizeof(pid_s), "%04x", pid);
                snprintf(devpath, sizeof(devpath), "/dev/%s", ent->d_name);
                cJSON_AddStringToObject(dev, "vid", vid_s);
                cJSON_AddStringToObject(dev, "pid", pid_s);
                cJSON_AddStringToObject(dev, "name", hid_name);
                cJSON_AddStringToObject(dev, "device", devpath);
                cJSON_AddItemToArray(usb, dev);
            }
        }
        closedir(dir);
    }
    cJSON_AddItemToObject(obj, "usb", usb);

    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return api_ok(json);
}

static api_response_t handle_setup_test(const api_request_t *req, void *ud)
{
    (void)ud;
    if (!req->body) return api_error(400, "request body required");

    cJSON *body = cJSON_Parse(req->body);
    if (!body) return api_error(400, "invalid JSON");

    const cJSON *jtype = cJSON_GetObjectItem(body, "conn_type");
    const char *conn_type = (jtype && cJSON_IsString(jtype)) ? jtype->valuestring : "serial";

    ups_conn_params_t params = {0};
    /* Own the device path locally — cJSON_Delete(body) below frees the
     * jdev->valuestring we'd otherwise hand to the driver, and ups_connect
     * doesn't deep-copy until *after* drv->connect() has already used it. */
    char device_buf[256] = {0};

    if (strcmp(conn_type, "usb") == 0) {
        const cJSON *jvid = cJSON_GetObjectItem(body, "usb_vid");
        const cJSON *jpid = cJSON_GetObjectItem(body, "usb_pid");
        params.type = UPS_CONN_USB;
        params.usb.vendor_id = (jvid && cJSON_IsString(jvid))
            ? (uint16_t)strtol(jvid->valuestring, NULL, 16) : 0x051d;
        params.usb.product_id = (jpid && cJSON_IsString(jpid))
            ? (uint16_t)strtol(jpid->valuestring, NULL, 16) : 0x0002;
    } else {
        const cJSON *jdev = cJSON_GetObjectItem(body, "device");
        const cJSON *jbaud = cJSON_GetObjectItem(body, "baud");
        const cJSON *jslave = cJSON_GetObjectItem(body, "slave_id");
        if (!jdev || !cJSON_IsString(jdev)) {
            cJSON_Delete(body);
            return api_error(400, "missing 'device' field");
        }
        snprintf(device_buf, sizeof(device_buf), "%s", jdev->valuestring);
        params.type = UPS_CONN_SERIAL;
        params.serial.device = device_buf;
        params.serial.baud = (jbaud && cJSON_IsNumber(jbaud)) ? jbaud->valueint : 9600;
        params.serial.slave_id = (jslave && cJSON_IsNumber(jslave)) ? jslave->valueint : 1;
    }
    cJSON_Delete(body);

    ups_t *test_ups = NULL;
    int cc = ups_connect(&params, &test_ups);
    if (cc != UPS_OK) {
        const char *hint = (cc == UPS_ERR_NO_DRIVER)
            ? "no driver recognized this UPS — check that Modbus is enabled and the slave address matches"
            : "failed to connect — check device path, baud rate, and that the UPS is powered on";
        return api_error(502, hint);
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "connected");
    cJSON_AddStringToObject(resp, "driver", ups_driver_name(test_ups));

    const char *topo = "unknown";
    switch (ups_topology(test_ups)) {
    case UPS_TOPO_ONLINE_DOUBLE:    topo = "online_double"; break;
    case UPS_TOPO_LINE_INTERACTIVE: topo = "line_interactive"; break;
    case UPS_TOPO_STANDBY:          topo = "standby"; break;
    }
    cJSON_AddStringToObject(resp, "topology", topo);

    if (test_ups->has_inventory) {
        cJSON *inv = cJSON_CreateObject();
        cJSON_AddStringToObject(inv, "model", test_ups->inventory.model);
        cJSON_AddStringToObject(inv, "serial", test_ups->inventory.serial);
        cJSON_AddStringToObject(inv, "firmware", test_ups->inventory.firmware);
        cJSON_AddNumberToObject(inv, "nominal_va", test_ups->inventory.nominal_va);
        cJSON_AddNumberToObject(inv, "nominal_watts", test_ups->inventory.nominal_watts);
        cJSON_AddItemToObject(resp, "inventory", inv);
    }

    ups_close(test_ups);

    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return api_ok(json);
}

/* --- Registration --- */

void api_register_auth_routes(api_server_t *srv, route_ctx_t *ctx)
{
    api_server_route(srv, "/api/auth/setup",   API_POST, handle_auth_setup,  ctx);
    api_server_route(srv, "/api/auth/login",   API_POST, handle_auth_login,  ctx);
    api_server_route(srv, "/api/auth/change",  API_POST, handle_auth_change, ctx);
    api_server_route(srv, "/api/auth/logout",  API_POST, handle_auth_logout, ctx);
    api_server_route(srv, "/api/setup/status",  API_GET,  handle_setup_status, ctx);
    api_server_route(srv, "/api/setup/ports",   API_GET,  handle_setup_ports,  ctx);
    api_server_route(srv, "/api/setup/test",    API_POST, handle_setup_test,   ctx);
}
