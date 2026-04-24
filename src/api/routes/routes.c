#include "api/routes/routes.h"
#include "config/app_config.h"
#include "ups/ups.h"
#include <cutils/log.h>
#include <cutils/error.h>
#include <cutils/json.h>
#include <cutils/mem.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* Propagate a JSON builder failure as 500 + cutils_get_error(). */
#define ADD_OR_FAIL(expr) \
    do { \
        if ((expr) != CUTILS_OK) return api_error(500, cutils_get_error()); \
    } while (0)

/* Propagate a JSON builder failure by returning the status code to the
 * enclosing helper (caller of the helper handles the api_error itself). */
#define CHECK_ADD(expr) \
    do { int _rv = (expr); if (_rv != CUTILS_OK) return _rv; } while (0)

/* --- Helpers --- */

api_response_t api_ok_msg(const char *msg)
{
    CUTILS_AUTO_JSON_RESP cutils_json_resp_t *resp = NULL;
    CUTILS_AUTOFREE       char               *json = NULL;
    size_t len;

    if (json_resp_new(&resp) != CUTILS_OK ||
        json_resp_add_str(resp, "result", msg ? msg : "") != CUTILS_OK ||
        json_resp_finalize(resp, &json, &len) != CUTILS_OK) {
        return api_error(500, cutils_get_error());
    }
    return api_ok(CUTILS_MOVE(json));
}

/* Finalize a response builder and hand the buffer off to api_ok. */
static api_response_t finalize_ok(cutils_json_resp_t *resp)
{
    CUTILS_AUTOFREE char *json = NULL;
    size_t len;
    if (json_resp_finalize(resp, &json, &len) != CUTILS_OK)
        return api_error(500, cutils_get_error());
    return api_ok(CUTILS_MOVE(json));
}

/* --- Helper: populate status object at root of resp --- */

static int build_status_into_resp(route_ctx_t *ctx, cutils_json_resp_t *resp)
{
    ups_data_t data;
    ups_inventory_t inv;

    const char *ups_name = config_get_str(ctx->config, "setup.ups_name");
    if (ups_name && ups_name[0])
        CHECK_ADD(json_resp_add_str(resp, "name", ups_name));

    if (!ctx->monitor || !ctx->ups) {
        CHECK_ADD(json_resp_add_str (resp, "driver",    "none"));
        CHECK_ADD(json_resp_add_bool(resp, "connected", false));
        CHECK_ADD(json_resp_add_str (resp, "message",   "no UPS connected"));
        return CUTILS_OK;
    }

    CHECK_ADD(json_resp_add_str (resp, "driver",    monitor_driver_name(ctx->monitor)));
    CHECK_ADD(json_resp_add_bool(resp, "connected", monitor_is_connected(ctx->monitor) != 0));

    const char *topo_str = "unknown";
    switch (ups_topology(ctx->ups)) {
    case UPS_TOPO_ONLINE_DOUBLE:    topo_str = "online_double"; break;
    case UPS_TOPO_LINE_INTERACTIVE: topo_str = "line_interactive"; break;
    case UPS_TOPO_STANDBY:          topo_str = "standby"; break;
    }
    CHECK_ADD(json_resp_add_str(resp, "topology", topo_str));

    if (monitor_get_inventory(ctx->monitor, &inv) == 0) {
        CHECK_ADD(json_resp_add_str(resp, "inventory.model",         inv.model));
        CHECK_ADD(json_resp_add_str(resp, "inventory.serial",        inv.serial));
        CHECK_ADD(json_resp_add_str(resp, "inventory.firmware",      inv.firmware));
        CHECK_ADD(json_resp_add_u32(resp, "inventory.nominal_va",    inv.nominal_va));
        CHECK_ADD(json_resp_add_u32(resp, "inventory.nominal_watts", inv.nominal_watts));
        CHECK_ADD(json_resp_add_u32(resp, "inventory.sog_config",    inv.sog_config));
    }

    if (monitor_get_status(ctx->monitor, &data) == 0) {
        char status_str[256];
        ups_status_str(data.status, status_str, sizeof(status_str));

        CHECK_ADD(json_resp_add_u32(resp, "status.raw",  data.status));
        CHECK_ADD(json_resp_add_str(resp, "status.text", status_str));

        CHECK_ADD(json_resp_add_f64(resp, "battery.charge_pct",  data.charge_pct));
        CHECK_ADD(json_resp_add_f64(resp, "battery.voltage",     data.battery_voltage));
        CHECK_ADD(json_resp_add_u32(resp, "battery.runtime_sec", data.runtime_sec));

        CHECK_ADD(json_resp_add_f64(resp, "output.voltage",   data.output_voltage));
        CHECK_ADD(json_resp_add_f64(resp, "output.frequency", data.output_frequency));
        CHECK_ADD(json_resp_add_f64(resp, "output.current",   data.output_current));
        CHECK_ADD(json_resp_add_f64(resp, "output.load_pct",  data.load_pct));
        CHECK_ADD(json_resp_add_u32(resp, "output.energy_wh", data.output_energy_wh));

        CHECK_ADD(json_resp_add_u32(resp, "outlets.mog",  data.outlet_mog));
        CHECK_ADD(json_resp_add_u32(resp, "outlets.sog0", data.outlet_sog0));
        CHECK_ADD(json_resp_add_u32(resp, "outlets.sog1", data.outlet_sog1));

        CHECK_ADD(json_resp_add_f64(resp, "bypass.voltage",      data.bypass_voltage));
        CHECK_ADD(json_resp_add_f64(resp, "bypass.frequency",    data.bypass_frequency));
        CHECK_ADD(json_resp_add_u32(resp, "bypass.status",       data.bypass_status));
        CHECK_ADD(json_resp_add_i32(resp, "bypass.voltage_high",
            config_get_int(ctx->config, "bypass.voltage_high", 140)));
        CHECK_ADD(json_resp_add_i32(resp, "bypass.voltage_low",
            config_get_int(ctx->config, "bypass.voltage_low", 90)));

        CHECK_ADD(json_resp_add_f64(resp, "input.voltage", data.input_voltage));
        CHECK_ADD(json_resp_add_u32(resp, "input.status",  data.input_status));
        if (ctx->transfer_high > 0) {
            CHECK_ADD(json_resp_add_i32(resp, "input.transfer_high", ctx->transfer_high));
            CHECK_ADD(json_resp_add_i32(resp, "input.transfer_low",  ctx->transfer_low));
            CHECK_ADD(json_resp_add_i32(resp, "input.warn_offset",
                config_get_int(ctx->config, "alerts.voltage_warn_offset", 5)));
        }

        CHECK_ADD(json_resp_add_f64 (resp, "efficiency.value",  data.efficiency));
        CHECK_ADD(json_resp_add_u32 (resp, "efficiency.reason", data.efficiency_reason));
        CHECK_ADD(json_resp_add_bool(resp, "efficiency.valid",
                                     data.efficiency_reason == UPS_EFF_OK));

        CHECK_ADD(json_resp_add_str(resp, "transfer_reason",
                                    ups_transfer_reason_str(data.transfer_reason)));

        /* Error registers */
        CHECK_ADD(json_resp_add_u32(resp, "errors.general",        data.general_error));
        CHECK_ADD(json_resp_add_u32(resp, "errors.power_system",   data.power_system_error));
        CHECK_ADD(json_resp_add_u32(resp, "errors.battery_system", data.bat_system_error));

        /* Detail arrays — always present, possibly empty. */
        CHECK_ADD(json_resp_ensure_array(resp, "errors.general_detail"));
        CHECK_ADD(json_resp_ensure_array(resp, "errors.power_system_detail"));
        CHECK_ADD(json_resp_ensure_array(resp, "errors.battery_system_detail"));

        const char *strs[16];
        int n;

        n = ups_decode_general_errors(data.general_error, strs, 16);
        for (int i = 0; i < n; i++)
            CHECK_ADD(json_resp_array_append_str(resp, "errors.general_detail", strs[i]));

        n = ups_decode_power_errors(data.power_system_error, strs, 16);
        for (int i = 0; i < n; i++)
            CHECK_ADD(json_resp_array_append_str(resp, "errors.power_system_detail", strs[i]));

        n = ups_decode_battery_errors(data.bat_system_error, strs, 16);
        for (int i = 0; i < n; i++)
            CHECK_ADD(json_resp_array_append_str(resp, "errors.battery_system_detail", strs[i]));

        CHECK_ADD(json_resp_add_u32(resp, "bat_test_status",      data.bat_test_status));
        CHECK_ADD(json_resp_add_u32(resp, "rt_cal_status",        data.rt_cal_status));
        CHECK_ADD(json_resp_add_u32(resp, "bat_lifetime_status",  data.bat_lifetime_status));

        CHECK_ADD(json_resp_add_i32(resp, "timers.shutdown", data.timer_shutdown));
        CHECK_ADD(json_resp_add_i32(resp, "timers.start",    data.timer_start));
        CHECK_ADD(json_resp_add_i32(resp, "timers.reboot",   data.timer_reboot));

        if (monitor_he_inhibit_active(ctx->monitor)) {
            CHECK_ADD(json_resp_add_bool(resp, "he_mode.inhibited", true));
            CHECK_ADD(json_resp_add_str (resp, "he_mode.source",
                                         monitor_he_inhibit_source(ctx->monitor)));
        }
    }

    /* Capabilities — always present, possibly empty. */
    CHECK_ADD(json_resp_ensure_array(resp, "capabilities"));
    if (ups_has_cap(ctx->ups, UPS_CAP_SHUTDOWN))
        CHECK_ADD(json_resp_array_append_str(resp, "capabilities", "shutdown"));
    if (ups_has_cap(ctx->ups, UPS_CAP_BATTERY_TEST))
        CHECK_ADD(json_resp_array_append_str(resp, "capabilities", "battery_test"));
    if (ups_has_cap(ctx->ups, UPS_CAP_RUNTIME_CAL))
        CHECK_ADD(json_resp_array_append_str(resp, "capabilities", "runtime_cal"));
    if (ups_has_cap(ctx->ups, UPS_CAP_CLEAR_FAULTS))
        CHECK_ADD(json_resp_array_append_str(resp, "capabilities", "clear_faults"));
    if (ups_has_cap(ctx->ups, UPS_CAP_MUTE))
        CHECK_ADD(json_resp_array_append_str(resp, "capabilities", "mute"));
    if (ups_has_cap(ctx->ups, UPS_CAP_BEEP))
        CHECK_ADD(json_resp_array_append_str(resp, "capabilities", "beep"));
    if (ups_has_cap(ctx->ups, UPS_CAP_BYPASS))
        CHECK_ADD(json_resp_array_append_str(resp, "capabilities", "bypass"));
    if (ups_has_cap(ctx->ups, UPS_CAP_FREQ_TOLERANCE))
        CHECK_ADD(json_resp_array_append_str(resp, "capabilities", "freq_tolerance"));
    if (ups_has_cap(ctx->ups, UPS_CAP_HE_MODE))
        CHECK_ADD(json_resp_array_append_str(resp, "capabilities", "he_mode"));

    return CUTILS_OK;
}

/* --- Core route handlers --- */

static api_response_t handle_version(const api_request_t *req, void *ud)
{
    (void)req;
    (void)ud;

    CUTILS_AUTO_JSON_RESP cutils_json_resp_t *resp = NULL;
    if (json_resp_new(&resp) != CUTILS_OK)
        return api_error(500, cutils_get_error());
#ifdef VERSION_STRING
    ADD_OR_FAIL(json_resp_add_str(resp, "daemon", VERSION_STRING));
#else
    ADD_OR_FAIL(json_resp_add_str(resp, "daemon", "unknown"));
#endif
    return finalize_ok(resp);
}

static api_response_t handle_status(const api_request_t *req, void *ud)
{
    (void)req;
    route_ctx_t *ctx = ud;

    CUTILS_AUTO_JSON_RESP cutils_json_resp_t *resp = NULL;
    if (json_resp_new(&resp) != CUTILS_OK)
        return api_error(500, cutils_get_error());
    ADD_OR_FAIL(build_status_into_resp(ctx, resp));
    return finalize_ok(resp);
}

static api_response_t handle_cmd(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;

    if (!ctx->ups)
        return api_error(503, "no UPS connected");
    if (!req->body || req->body_len == 0)
        return api_error(400, "request body required");

    CUTILS_AUTO_JSON_REQ cutils_json_req_t *body = NULL;
    if (json_req_parse(req->body, req->body_len, &body) != CUTILS_OK)
        return api_error(400, "invalid JSON");

    CUTILS_AUTOFREE char *act = NULL;
    if (json_req_get_str(body, "action", &act) != CUTILS_OK)
        return api_error(400, "missing 'action' field");

    int rc;
    const char *result_msg = "ok";

    if (strcmp(act, "shutdown_workflow") == 0) {
        bool dry_run = false;
        CUTILS_UNUSED(json_req_get_bool(body, "dry_run", &dry_run));
        rc = shutdown_execute(ctx->shutdown, dry_run ? 1 : 0);
        result_msg = dry_run ? "dry run complete" : "shutdown initiated";
    } else {
        const ups_cmd_desc_t *cmd = ups_find_command(ctx->ups, act);
        if (!cmd)
            return api_error(400, "unknown action");

        int is_off = 0;
        if (cmd->type == UPS_CMD_TOGGLE) {
            bool off_flag = false;
            CUTILS_UNUSED(json_req_get_bool(body, "off", &off_flag));
            is_off = off_flag ? 1 : 0;
        }

        rc = ups_cmd_execute(ctx->ups, act, is_off);
        result_msg = cmd->display_name;
    }

    if (rc == UPS_ERR_NOT_SUPPORTED)
        return api_error(400, "command not supported by this UPS");
    if (rc != 0)
        return api_error(500, "command failed");

    return api_ok_msg(result_msg);
}

static api_response_t handle_events(const api_request_t *req, void *ud)
{
    (void)req;
    route_ctx_t *ctx = ud;

    CUTILS_AUTO_DBRES db_result_t *result = NULL;
    int rc = db_execute(ctx->db,
        "SELECT timestamp, severity, category, title, message "
        "FROM events ORDER BY id DESC LIMIT 50",
        NULL, &result);

    if (rc != CUTILS_OK || !result)
        return api_error(500, "failed to query events");

    CUTILS_AUTO_JSON_RESP cutils_json_resp_t *resp = NULL;
    if (json_resp_new_array(&resp) != CUTILS_OK)
        return api_error(500, cutils_get_error());

    for (int i = 0; i < result->nrows; i++) {
        CUTILS_AUTO_JSON_ELEM cutils_json_elem_t ev;
        ADD_OR_FAIL(json_resp_array_append_begin(resp, "", &ev));
        ADD_OR_FAIL(json_elem_add_str(&ev, "timestamp", result->rows[i][0]));
        ADD_OR_FAIL(json_elem_add_str(&ev, "severity",  result->rows[i][1]));
        ADD_OR_FAIL(json_elem_add_str(&ev, "category",  result->rows[i][2]));
        ADD_OR_FAIL(json_elem_add_str(&ev, "title",     result->rows[i][3]));
        ADD_OR_FAIL(json_elem_add_str(&ev, "message",   result->rows[i][4]));
        json_elem_commit(&ev);
    }

    return finalize_ok(resp);
}

static api_response_t handle_telemetry(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;

    const char *from_param  = api_query_param(req, "from");
    const char *to_param    = api_query_param(req, "to");
    const char *limit_param = api_query_param(req, "limit");

    int limit = limit_param ? atoi(limit_param) : 500;
    if (limit <= 0 || limit > 10000) limit = 500;

    char limit_s[16];
    snprintf(limit_s, sizeof(limit_s), "%d", limit);

    CUTILS_AUTO_DBRES db_result_t *result = NULL;
    int rc;

    if (from_param && to_param) {
        const char *params[] = { from_param, to_param, limit_s, NULL };
        rc = db_execute(ctx->db,
            "SELECT timestamp, status, charge_pct, runtime_sec, battery_voltage, "
            "load_pct, output_voltage, output_frequency, output_current, "
            "input_voltage, efficiency "
            "FROM telemetry WHERE timestamp >= ? AND timestamp <= ? "
            "ORDER BY id ASC LIMIT ?",
            params, &result);
    } else if (from_param) {
        const char *params[] = { from_param, limit_s, NULL };
        rc = db_execute(ctx->db,
            "SELECT timestamp, status, charge_pct, runtime_sec, battery_voltage, "
            "load_pct, output_voltage, output_frequency, output_current, "
            "input_voltage, efficiency "
            "FROM telemetry WHERE timestamp >= ? ORDER BY id ASC LIMIT ?",
            params, &result);
    } else {
        const char *params[] = { limit_s, NULL };
        rc = db_execute(ctx->db,
            "SELECT timestamp, status, charge_pct, runtime_sec, battery_voltage, "
            "load_pct, output_voltage, output_frequency, output_current, "
            "input_voltage, efficiency "
            "FROM telemetry ORDER BY id DESC LIMIT ?",
            params, &result);
    }

    if (rc != CUTILS_OK || !result)
        return api_error(500, "failed to query telemetry");

    CUTILS_AUTO_JSON_RESP cutils_json_resp_t *resp = NULL;
    if (json_resp_new_array(&resp) != CUTILS_OK)
        return api_error(500, cutils_get_error());

    for (int i = 0; i < result->nrows; i++) {
        CUTILS_AUTO_JSON_ELEM cutils_json_elem_t pt;
        ADD_OR_FAIL(json_resp_array_append_begin(resp, "", &pt));
        ADD_OR_FAIL(json_elem_add_str(&pt, "timestamp",        result->rows[i][0]));
        ADD_OR_FAIL(json_elem_add_f64(&pt, "status",           atof(result->rows[i][1])));
        ADD_OR_FAIL(json_elem_add_f64(&pt, "charge_pct",       atof(result->rows[i][2])));
        ADD_OR_FAIL(json_elem_add_f64(&pt, "runtime_sec",      atof(result->rows[i][3])));
        ADD_OR_FAIL(json_elem_add_f64(&pt, "battery_voltage",  atof(result->rows[i][4])));
        ADD_OR_FAIL(json_elem_add_f64(&pt, "load_pct",         atof(result->rows[i][5])));
        ADD_OR_FAIL(json_elem_add_f64(&pt, "output_voltage",   atof(result->rows[i][6])));
        ADD_OR_FAIL(json_elem_add_f64(&pt, "output_frequency", atof(result->rows[i][7])));
        ADD_OR_FAIL(json_elem_add_f64(&pt, "output_current",   atof(result->rows[i][8])));
        ADD_OR_FAIL(json_elem_add_f64(&pt, "input_voltage",    atof(result->rows[i][9])));
        ADD_OR_FAIL(json_elem_add_f64(&pt, "efficiency",       atof(result->rows[i][10])));
        json_elem_commit(&pt);
    }

    return finalize_ok(resp);
}

static api_response_t handle_commands(const api_request_t *req, void *ud)
{
    (void)req;
    route_ctx_t *ctx = ud;

    CUTILS_AUTO_JSON_RESP cutils_json_resp_t *resp = NULL;
    if (json_resp_new_array(&resp) != CUTILS_OK)
        return api_error(500, cutils_get_error());

    if (ctx->ups) {
        size_t count;
        const ups_cmd_desc_t *cmds = ups_get_commands(ctx->ups, &count);
        for (size_t i = 0; i < count; i++) {
            CUTILS_AUTO_JSON_ELEM cutils_json_elem_t c;
            ADD_OR_FAIL(json_resp_array_append_begin(resp, "", &c));
            ADD_OR_FAIL(json_elem_add_str(&c, "name",          cmds[i].name));
            ADD_OR_FAIL(json_elem_add_str(&c, "display_name",  cmds[i].display_name));
            ADD_OR_FAIL(json_elem_add_str(&c, "description",   cmds[i].description));
            ADD_OR_FAIL(json_elem_add_str(&c, "group",         cmds[i].group));
            ADD_OR_FAIL(json_elem_add_str(&c, "confirm_title", cmds[i].confirm_title));
            ADD_OR_FAIL(json_elem_add_str(&c, "confirm_body",  cmds[i].confirm_body));
            ADD_OR_FAIL(json_elem_add_str(&c, "type",
                cmds[i].type == UPS_CMD_TOGGLE ? "toggle" : "simple"));
            ADD_OR_FAIL(json_elem_add_str(&c, "variant",
                cmds[i].variant == UPS_CMD_DANGER ? "danger" :
                cmds[i].variant == UPS_CMD_WARN   ? "warn"   : "default"));
            if (cmds[i].flags & UPS_CMD_IS_SHUTDOWN)
                ADD_OR_FAIL(json_elem_add_bool(&c, "is_shutdown", true));
            if (cmds[i].flags & UPS_CMD_IS_MUTE)
                ADD_OR_FAIL(json_elem_add_bool(&c, "is_mute", true));
            if (cmds[i].type == UPS_CMD_TOGGLE)
                ADD_OR_FAIL(json_elem_add_u32(&c, "status_bit", cmds[i].status_bit));
            json_elem_commit(&c);
        }
    }

    return finalize_ok(resp);
}

/* --- Restart endpoint --- */

extern void app_request_restart(void);

static api_response_t handle_restart(const api_request_t *req, void *ud)
{
    (void)req;
    (void)ud;

    log_info("restart requested via API");
    app_request_restart();
    return api_ok_msg("restarting");
}

/* --- Threshold cache --- */

void api_refresh_thresholds(route_ctx_t *ctx)
{
    if (!ctx->ups) return;
    ups_read_thresholds(ctx->ups, &ctx->transfer_high, &ctx->transfer_low);
}

#undef ADD_OR_FAIL
#undef CHECK_ADD

/* --- Route registration --- */

void api_register_routes(api_server_t *srv, route_ctx_t *ctx)
{
    api_refresh_thresholds(ctx);

    /* Core */
    api_server_route(srv, "/api/version",      API_GET,  handle_version,   ctx);
    api_server_route(srv, "/api/status",       API_GET,  handle_status,    ctx);
    api_server_route(srv, "/api/commands",     API_GET,  handle_commands,  ctx);
    api_server_route(srv, "/api/cmd",          API_POST, handle_cmd,       ctx);
    api_server_route(srv, "/api/events",       API_GET,  handle_events,    ctx);
    api_server_route(srv, "/api/telemetry",    API_GET,  handle_telemetry, ctx);
    api_server_route(srv, "/api/restart",      API_POST, handle_restart,   ctx);

    /* Domain modules */
    api_register_auth_routes(srv, ctx);
    api_register_shutdown_routes(srv, ctx);
    api_register_config_routes(srv, ctx);
    api_register_weather_routes(srv, ctx);
}
