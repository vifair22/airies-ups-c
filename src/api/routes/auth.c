#include "api/routes/routes.h"
#include <cutils/error.h>
#include <cutils/json.h>
#include <cutils/mem.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>

/* Propagate a JSON builder failure as 500 + cutils_get_error(). */
#define ADD_OR_FAIL(expr) \
    do { \
        if ((expr) != CUTILS_OK) return api_error(500, cutils_get_error()); \
    } while (0)

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
    /* Best-effort audit trail; a failure here must not block whatever
     * auth operation triggered this event. */
    CUTILS_UNUSED(db_execute_non_query(db,
        "INSERT INTO events (timestamp, severity, category, title, message) "
        "VALUES (?, ?, ?, ?, ?)",
        params, NULL));
}

/* Serialize a resp builder and hand the buffer off to api_ok. */
static api_response_t finalize_ok(cutils_json_resp_t *resp)
{
    CUTILS_AUTOFREE char *json = NULL;
    size_t len;
    if (json_resp_finalize(resp, &json, &len) != CUTILS_OK)
        return api_error(500, cutils_get_error());
    return api_ok(CUTILS_MOVE(json));
}


/* --- Auth endpoints --- */

static api_response_t handle_auth_setup(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;
    if (auth_is_setup(ctx->db))
        return api_error(400, "admin password already set");

    if (!req->body) return api_error(400, "request body required");

    CUTILS_AUTO_JSON_REQ cutils_json_req_t *body = NULL;
    if (json_req_parse(req->body, req->body_len, &body) != CUTILS_OK)
        return api_error(400, "invalid JSON");

    CUTILS_AUTOFREE char *password = NULL;
    if (json_req_get_str(body, "password", &password) != CUTILS_OK ||
        strlen(password) < 4)
        return api_error(400, "password must be at least 4 characters");

    if (auth_set_password(ctx->db, password) != 0)
        return api_error(500, cutils_get_error());
    auth_event(ctx->db, "info", "Password Set", "Admin password configured during initial setup");

    CUTILS_AUTO_JSON_RESP cutils_json_resp_t *resp = NULL;
    if (json_resp_new(&resp) != CUTILS_OK)
        return api_error(500, cutils_get_error());
    ADD_OR_FAIL(json_resp_add_str(resp, "result", "password set"));
    return finalize_ok(resp);
}

static api_response_t handle_auth_login(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;
    if (!req->body) return api_error(400, "request body required");

    CUTILS_AUTO_JSON_REQ cutils_json_req_t *body = NULL;
    if (json_req_parse(req->body, req->body_len, &body) != CUTILS_OK)
        return api_error(400, "invalid JSON");

    CUTILS_AUTOFREE char *password = NULL;
    if (json_req_get_str(body, "password", &password) != CUTILS_OK)
        return api_error(400, "missing 'password'");

    if (!auth_verify_password(ctx->db, password)) {
        auth_event(ctx->db, "warning", "Login Failed", "Invalid password attempt");
        return api_error(401, "invalid password");
    }
    auth_event(ctx->db, "info", "Login", "Admin login successful");

    CUTILS_AUTOFREE char *token = auth_create_token(ctx->db, AUTH_SESSION_TTL_HOURS);
    if (!token) return api_error(500, "failed to create session");

    /* Issue an HttpOnly cookie alongside the JSON token. The cookie is
     * what the browser uses going forward (and is the only way SSE auth
     * works without a header — EventSource can't send Authorization).
     * The token in the JSON body remains for non-browser clients (CLI,
     * curl, integrations) that prefer Authorization-header auth. */
    int cookie_secure = config_get_int(ctx->config, "auth.cookie_secure", 0);
    char *set_cookie = api_cookie_set("auth", token,
                                      AUTH_SESSION_TTL_HOURS * 3600, cookie_secure);

    CUTILS_AUTO_JSON_RESP cutils_json_resp_t *resp = NULL;
    if (json_resp_new(&resp) != CUTILS_OK) {
        free(set_cookie);
        return api_error(500, cutils_get_error());
    }
    ADD_OR_FAIL(json_resp_add_str(resp, "token",      token));
    ADD_OR_FAIL(json_resp_add_u32(resp, "expires_in",
                                  (uint32_t)AUTH_SESSION_TTL_HOURS * 3600U));
    api_response_t out = finalize_ok(resp);
    out.set_cookie = set_cookie;
    return out;
}

static api_response_t handle_auth_change(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;
    if (!req->body) return api_error(400, "request body required");

    CUTILS_AUTO_JSON_REQ cutils_json_req_t *body = NULL;
    if (json_req_parse(req->body, req->body_len, &body) != CUTILS_OK)
        return api_error(400, "invalid JSON");

    CUTILS_AUTOFREE char *old_pw = NULL;
    CUTILS_AUTOFREE char *new_pw = NULL;
    if (json_req_get_str(body, "old_password", &old_pw) != CUTILS_OK ||
        json_req_get_str(body, "new_password", &new_pw) != CUTILS_OK)
        return api_error(400, "missing 'old_password' and 'new_password'");
    if (strlen(new_pw) < 4)
        return api_error(400, "new password must be at least 4 characters");
    if (!auth_verify_password(ctx->db, old_pw))
        return api_error(401, "current password is incorrect");

    int rc = auth_set_password(ctx->db, new_pw);
    if (rc != 0) return api_error(500, "failed to update password");

    auth_event(ctx->db, "info", "Password Changed", "Admin password updated");

    CUTILS_AUTO_JSON_RESP cutils_json_resp_t *resp = NULL;
    if (json_resp_new(&resp) != CUTILS_OK)
        return api_error(500, cutils_get_error());
    ADD_OR_FAIL(json_resp_add_str(resp, "result", "password changed"));
    return finalize_ok(resp);
}

static api_response_t handle_auth_logout(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;

    /* Revoke whichever token authenticated this request — header first,
     * cookie as fallback. Either path validates as the same DB row, so
     * one DELETE suffices regardless of how the caller authenticated. */
    const char *tok = req->auth_token;
    if (!tok || !tok[0])
        tok = api_cookie(req, "auth");
    auth_revoke_token(ctx->db, tok);

    auth_event(ctx->db, "info", "Logout", "Admin session ended");

    int cookie_secure = config_get_int(ctx->config, "auth.cookie_secure", 0);
    char *clear_cookie = api_cookie_clear("auth", cookie_secure);

    CUTILS_AUTO_JSON_RESP cutils_json_resp_t *resp = NULL;
    if (json_resp_new(&resp) != CUTILS_OK) {
        free(clear_cookie);
        return api_error(500, cutils_get_error());
    }
    ADD_OR_FAIL(json_resp_add_str(resp, "result", "logged out"));
    api_response_t out = finalize_ok(resp);
    out.set_cookie = clear_cookie;
    return out;
}

/* Lightweight "am I logged in?" probe. The auth middleware runs before
 * this handler, so reaching it at all means the request was authorized.
 * Returns 200 with a tiny body the frontend can ignore. The frontend's
 * AuthGuard hits this on mount to decide whether to show the login
 * page — it can't read the HttpOnly cookie directly. */
static api_response_t handle_auth_check(const api_request_t *req, void *ud)
{
    (void)req; (void)ud;
    CUTILS_AUTO_JSON_RESP cutils_json_resp_t *resp = NULL;
    if (json_resp_new(&resp) != CUTILS_OK)
        return api_error(500, cutils_get_error());
    ADD_OR_FAIL(json_resp_add_bool(resp, "ok", 1));
    return finalize_ok(resp);
}

/* --- Setup endpoints --- */

static api_response_t handle_setup_status(const api_request_t *req, void *ud)
{
    (void)req;
    route_ctx_t *ctx = ud;

    int password_set   = auth_is_setup(ctx->db);
    int ups_configured = config_get_int(ctx->config, "setup.ups_done", 0) != 0;

    CUTILS_AUTO_JSON_RESP cutils_json_resp_t *resp = NULL;
    if (json_resp_new(&resp) != CUTILS_OK)
        return api_error(500, cutils_get_error());
    ADD_OR_FAIL(json_resp_add_bool(resp, "needs_setup",
                                   !password_set || !ups_configured));
    ADD_OR_FAIL(json_resp_add_bool(resp, "password_set",   password_set != 0));
    ADD_OR_FAIL(json_resp_add_bool(resp, "ups_configured", ups_configured != 0));
    ADD_OR_FAIL(json_resp_add_bool(resp, "ups_connected",
                                   ctx->ups && monitor_is_connected(ctx->monitor)));
    return finalize_ok(resp);
}

static api_response_t handle_setup_ports(const api_request_t *req, void *ud)
{
    (void)req;
    (void)ud;

    CUTILS_AUTO_JSON_RESP cutils_json_resp_t *resp = NULL;
    if (json_resp_new(&resp) != CUTILS_OK)
        return api_error(500, cutils_get_error());

    /* Both arrays are always present in the response, even if empty —
     * match the original cJSON behavior. */
    ADD_OR_FAIL(json_resp_ensure_array(resp, "serial"));
    ADD_OR_FAIL(json_resp_ensure_array(resp, "usb"));

    /* Scan for serial devices — scalar-append builds the 'serial' array. */
    const char *patterns[] = { "/dev/ttyUSB", "/dev/ttyACM", "/dev/ttyS", NULL };
    for (int p = 0; patterns[p]; p++) {
        for (int i = 0; i < 10; i++) {
            char path[64];
            snprintf(path, sizeof(path), "%s%d", patterns[p], i);
            /* nosemgrep: flawfinder.access-1 -- discovery probe for the setup wizard's device list; no follow-up open on `path` so there is no TOCTOU window */
            if (access(path, R_OK | W_OK) == 0)
                ADD_OR_FAIL(json_resp_array_append_str(resp, "serial", path));
        }
    }

    /* Scan for USB HID power devices — element builder for array-of-objects. */
    DIR *dir = opendir("/sys/class/hidraw");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            char uevent[512];
            snprintf(uevent, sizeof(uevent),
                     "/sys/class/hidraw/%s/device/uevent", ent->d_name);
            CUTILS_AUTOCLOSE FILE *f = fopen(uevent, "r");
            if (!f) continue;

            char line[256];
            unsigned int vid = 0, pid = 0;
            char hid_name[128] = "";
            while (fgets(line, sizeof(line), f)) {
                unsigned int bus;
                /* nosemgrep: flawfinder.fscanf-1.sscanf-1.vsscanf-1.vfscanf-1._ftscanf-1.fwscanf-1.vfwscanf-1.vswscanf-1 -- literal format with only %x integer specifiers */
                if (sscanf(line, "HID_ID=%x:%x:%x", &bus, &vid, &pid) == 3)
                    continue;
                /* nosemgrep: flawfinder.fscanf-1.sscanf-1.vsscanf-1.vfscanf-1._ftscanf-1.fwscanf-1.vfwscanf-1.vswscanf-1 -- %127[^\n] width-bounded into 128-byte buffer */
                if (sscanf(line, "HID_NAME=%127[^\n]", hid_name) == 1)
                    continue;
            }

            if (vid > 0) {
                CUTILS_AUTO_JSON_ELEM cutils_json_elem_t elem;
                if (json_resp_array_append_begin(resp, "usb", &elem) != CUTILS_OK) {
                    closedir(dir);
                    return api_error(500, cutils_get_error());
                }
                char vid_s[8], pid_s[8], devpath[512];
                snprintf(vid_s,   sizeof(vid_s),   "%04x", vid);
                snprintf(pid_s,   sizeof(pid_s),   "%04x", pid);
                snprintf(devpath, sizeof(devpath), "/dev/%s", ent->d_name);
                if (json_elem_add_str(&elem, "vid",    vid_s)    != CUTILS_OK ||
                    json_elem_add_str(&elem, "pid",    pid_s)    != CUTILS_OK ||
                    json_elem_add_str(&elem, "name",   hid_name) != CUTILS_OK ||
                    json_elem_add_str(&elem, "device", devpath)  != CUTILS_OK) {
                    closedir(dir);
                    return api_error(500, cutils_get_error());
                }
                json_elem_commit(&elem);
            }
        }
        closedir(dir);
    }

    return finalize_ok(resp);
}

static api_response_t handle_setup_test(const api_request_t *req, void *ud)
{
    (void)ud;
    if (!req->body) return api_error(400, "request body required");

    CUTILS_AUTO_JSON_REQ cutils_json_req_t *body = NULL;
    if (json_req_parse(req->body, req->body_len, &body) != CUTILS_OK)
        return api_error(400, "invalid JSON");

    CUTILS_AUTOFREE char *conn_type = NULL;
    CUTILS_UNUSED(json_req_get_str_opt(body, "conn_type", &conn_type));
    const char *type = conn_type ? conn_type : "serial";

    /* Owned strings from the request survive json_req_free, so there's no
     * more UAF-through-valuestring risk here — the previous workaround
     * (device_buf) is no longer needed. */
    ups_conn_params_t params = {0};
    CUTILS_AUTOFREE char *device = NULL;
    CUTILS_AUTOFREE char *vid_str = NULL;
    CUTILS_AUTOFREE char *pid_str = NULL;

    if (strcmp(type, "usb") == 0) {
        CUTILS_UNUSED(json_req_get_str_opt(body, "usb_vid", &vid_str));
        CUTILS_UNUSED(json_req_get_str_opt(body, "usb_pid", &pid_str));
        params.type = UPS_CONN_USB;
        params.usb.vendor_id  = vid_str ? (uint16_t)strtol(vid_str, NULL, 16) : 0x051d;
        params.usb.product_id = pid_str ? (uint16_t)strtol(pid_str, NULL, 16) : 0x0002;
    } else {
        if (json_req_get_str(body, "device", &device) != CUTILS_OK)
            return api_error(400, "missing 'device' field");
        int32_t baud     = 9600;
        int32_t slave_id = 1;
        CUTILS_UNUSED(json_req_get_i32(body, "baud",     &baud,     0, INT32_MAX));
        CUTILS_UNUSED(json_req_get_i32(body, "slave_id", &slave_id, 0, INT32_MAX));
        params.type = UPS_CONN_SERIAL;
        params.serial.device   = device;
        params.serial.baud     = baud;
        params.serial.slave_id = slave_id;
    }

    ups_t *test_ups = NULL;
    int cc = ups_connect(&params, &test_ups);
    if (cc != UPS_OK) {
        const char *hint = (cc == UPS_ERR_NO_DRIVER)
            ? "no driver recognized this UPS — check that Modbus is enabled and the slave address matches"
            : "failed to connect — check device path, baud rate, and that the UPS is powered on";
        return api_error(502, hint);
    }

    CUTILS_AUTO_JSON_RESP cutils_json_resp_t *resp = NULL;
    if (json_resp_new(&resp) != CUTILS_OK) {
        ups_close(test_ups);
        return api_error(500, cutils_get_error());
    }

    const char *topo;
    switch (ups_topology(test_ups)) {
    case UPS_TOPO_ONLINE_DOUBLE:    topo = "online_double"; break;
    case UPS_TOPO_LINE_INTERACTIVE: topo = "line_interactive"; break;
    case UPS_TOPO_STANDBY:          topo = "standby"; break;
    default:                        topo = "unknown"; break;
    }

    if (json_resp_add_str(resp, "result",   "connected")               != CUTILS_OK ||
        json_resp_add_str(resp, "driver",   ups_driver_name(test_ups)) != CUTILS_OK ||
        json_resp_add_str(resp, "topology", topo)                      != CUTILS_OK) {
        ups_close(test_ups);
        return api_error(500, cutils_get_error());
    }

    if (test_ups->has_inventory) {
        if (json_resp_add_str(resp, "inventory.model",         test_ups->inventory.model)         != CUTILS_OK ||
            json_resp_add_str(resp, "inventory.serial",        test_ups->inventory.serial)        != CUTILS_OK ||
            json_resp_add_str(resp, "inventory.firmware",      test_ups->inventory.firmware)      != CUTILS_OK ||
            json_resp_add_u32(resp, "inventory.nominal_va",    test_ups->inventory.nominal_va)    != CUTILS_OK ||
            json_resp_add_u32(resp, "inventory.nominal_watts", test_ups->inventory.nominal_watts) != CUTILS_OK) {
            ups_close(test_ups);
            return api_error(500, cutils_get_error());
        }
    }

    ups_close(test_ups);
    return finalize_ok(resp);
}

#undef ADD_OR_FAIL

/* --- Registration --- */

void api_register_auth_routes(api_server_t *srv, route_ctx_t *ctx)
{
    api_server_route(srv, "/api/auth/setup",   API_POST, handle_auth_setup,  ctx);
    api_server_route(srv, "/api/auth/login",   API_POST, handle_auth_login,  ctx);
    api_server_route(srv, "/api/auth/change",  API_POST, handle_auth_change, ctx);
    api_server_route(srv, "/api/auth/logout",  API_POST, handle_auth_logout, ctx);
    api_server_route(srv, "/api/auth/check",   API_GET,  handle_auth_check,  ctx);
    api_server_route(srv, "/api/setup/status",  API_GET,  handle_setup_status, ctx);
    api_server_route(srv, "/api/setup/ports",   API_GET,  handle_setup_ports,  ctx);
    api_server_route(srv, "/api/setup/test",    API_POST, handle_setup_test,   ctx);
}
