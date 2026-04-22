#include "api/routes/routes.h"
#include "config/app_config.h"
#include "ups/ups.h"
#include <cutils/log.h>
#include <cJSON.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* --- Helpers --- */

api_response_t api_ok_msg(const char *msg)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "result", msg);
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return api_ok(json);
}

/* --- Helper: build JSON status object --- */

static cJSON *build_status_json(route_ctx_t *ctx)
{
    cJSON *obj = cJSON_CreateObject();
    ups_data_t data;
    ups_inventory_t inv;

    const char *ups_name = config_get_str(ctx->config, "setup.ups_name");
    if (ups_name && ups_name[0])
        cJSON_AddStringToObject(obj, "name", ups_name);

    if (!ctx->monitor || !ctx->ups) {
        cJSON_AddStringToObject(obj, "driver", "none");
        cJSON_AddBoolToObject(obj, "connected", 0);
        cJSON_AddStringToObject(obj, "message", "no UPS connected");
        return obj;
    }

    cJSON_AddStringToObject(obj, "driver", monitor_driver_name(ctx->monitor));
    cJSON_AddBoolToObject(obj, "connected", monitor_is_connected(ctx->monitor));

    /* Topology */
    const char *topo_str = "unknown";
    switch (ups_topology(ctx->ups)) {
    case UPS_TOPO_ONLINE_DOUBLE:    topo_str = "online_double"; break;
    case UPS_TOPO_LINE_INTERACTIVE: topo_str = "line_interactive"; break;
    case UPS_TOPO_STANDBY:          topo_str = "standby"; break;
    }
    cJSON_AddStringToObject(obj, "topology", topo_str);

    if (monitor_get_inventory(ctx->monitor, &inv) == 0) {
        cJSON *inv_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(inv_obj, "model", inv.model);
        cJSON_AddStringToObject(inv_obj, "serial", inv.serial);
        cJSON_AddStringToObject(inv_obj, "firmware", inv.firmware);
        cJSON_AddNumberToObject(inv_obj, "nominal_va", inv.nominal_va);
        cJSON_AddNumberToObject(inv_obj, "nominal_watts", inv.nominal_watts);
        cJSON_AddNumberToObject(inv_obj, "sog_config", inv.sog_config);
        cJSON_AddItemToObject(obj, "inventory", inv_obj);
    }

    if (monitor_get_status(ctx->monitor, &data) == 0) {
        char status_str[256];
        ups_status_str(data.status, status_str, sizeof(status_str));

        cJSON *st = cJSON_CreateObject();
        cJSON_AddNumberToObject(st, "raw", data.status);
        cJSON_AddStringToObject(st, "text", status_str);
        cJSON_AddItemToObject(obj, "status", st);

        cJSON *bat = cJSON_CreateObject();
        cJSON_AddNumberToObject(bat, "charge_pct", data.charge_pct);
        cJSON_AddNumberToObject(bat, "voltage", data.battery_voltage);
        cJSON_AddNumberToObject(bat, "runtime_sec", data.runtime_sec);
        cJSON_AddItemToObject(obj, "battery", bat);

        cJSON *out = cJSON_CreateObject();
        cJSON_AddNumberToObject(out, "voltage", data.output_voltage);
        cJSON_AddNumberToObject(out, "frequency", data.output_frequency);
        cJSON_AddNumberToObject(out, "current", data.output_current);
        cJSON_AddNumberToObject(out, "load_pct", data.load_pct);
        cJSON_AddNumberToObject(out, "energy_wh", data.output_energy_wh);
        cJSON_AddItemToObject(obj, "output", out);

        cJSON *outlets = cJSON_CreateObject();
        cJSON_AddNumberToObject(outlets, "mog", data.outlet_mog);
        cJSON_AddNumberToObject(outlets, "sog0", data.outlet_sog0);
        cJSON_AddNumberToObject(outlets, "sog1", data.outlet_sog1);
        cJSON_AddItemToObject(obj, "outlets", outlets);

        cJSON *byp = cJSON_CreateObject();
        cJSON_AddNumberToObject(byp, "voltage", data.bypass_voltage);
        cJSON_AddNumberToObject(byp, "frequency", data.bypass_frequency);
        cJSON_AddNumberToObject(byp, "status", data.bypass_status);
        cJSON_AddNumberToObject(byp, "voltage_high",
            config_get_int(ctx->config, "bypass.voltage_high", 140));
        cJSON_AddNumberToObject(byp, "voltage_low",
            config_get_int(ctx->config, "bypass.voltage_low", 90));
        cJSON_AddItemToObject(obj, "bypass", byp);

        cJSON *in = cJSON_CreateObject();
        cJSON_AddNumberToObject(in, "voltage", data.input_voltage);
        cJSON_AddNumberToObject(in, "status", data.input_status);
        if (ctx->transfer_high > 0) {
            cJSON_AddNumberToObject(in, "transfer_high", ctx->transfer_high);
            cJSON_AddNumberToObject(in, "transfer_low", ctx->transfer_low);
            cJSON_AddNumberToObject(in, "warn_offset",
                config_get_int(ctx->config, "alerts.voltage_warn_offset", 5));
        }
        cJSON_AddItemToObject(obj, "input", in);

        {
            cJSON *eff = cJSON_CreateObject();
            cJSON_AddNumberToObject(eff, "value", data.efficiency);
            cJSON_AddNumberToObject(eff, "reason", (double)data.efficiency_reason);
            cJSON_AddBoolToObject(eff, "valid", data.efficiency_reason == UPS_EFF_OK);
            cJSON_AddItemToObject(obj, "efficiency", eff);
        }
        cJSON_AddStringToObject(obj, "transfer_reason",
                                ups_transfer_reason_str(data.transfer_reason));

        /* Error registers */
        {
            cJSON *errors = cJSON_CreateObject();
            cJSON_AddNumberToObject(errors, "general", data.general_error);
            cJSON_AddNumberToObject(errors, "power_system", data.power_system_error);
            cJSON_AddNumberToObject(errors, "battery_system", data.bat_system_error);

            const char *strs[16];
            int n;

            n = ups_decode_general_errors(data.general_error, strs, 16);
            cJSON *gen_arr = cJSON_CreateArray();
            for (int i = 0; i < n; i++) cJSON_AddItemToArray(gen_arr, cJSON_CreateString(strs[i]));
            cJSON_AddItemToObject(errors, "general_detail", gen_arr);

            n = ups_decode_power_errors(data.power_system_error, strs, 16);
            cJSON *pwr_arr = cJSON_CreateArray();
            for (int i = 0; i < n; i++) cJSON_AddItemToArray(pwr_arr, cJSON_CreateString(strs[i]));
            cJSON_AddItemToObject(errors, "power_system_detail", pwr_arr);

            n = ups_decode_battery_errors(data.bat_system_error, strs, 16);
            cJSON *bat_arr = cJSON_CreateArray();
            for (int i = 0; i < n; i++) cJSON_AddItemToArray(bat_arr, cJSON_CreateString(strs[i]));
            cJSON_AddItemToObject(errors, "battery_system_detail", bat_arr);

            cJSON_AddItemToObject(obj, "errors", errors);
        }

        cJSON_AddNumberToObject(obj, "bat_test_status", data.bat_test_status);
        cJSON_AddNumberToObject(obj, "rt_cal_status", data.rt_cal_status);
        cJSON_AddNumberToObject(obj, "bat_lifetime_status", data.bat_lifetime_status);

        cJSON *timers = cJSON_CreateObject();
        cJSON_AddNumberToObject(timers, "shutdown", data.timer_shutdown);
        cJSON_AddNumberToObject(timers, "start", data.timer_start);
        cJSON_AddNumberToObject(timers, "reboot", data.timer_reboot);
        cJSON_AddItemToObject(obj, "timers", timers);

        if (monitor_he_inhibit_active(ctx->monitor)) {
            cJSON *he = cJSON_CreateObject();
            cJSON_AddBoolToObject(he, "inhibited", 1);
            cJSON_AddStringToObject(he, "source",
                                    monitor_he_inhibit_source(ctx->monitor));
            cJSON_AddItemToObject(obj, "he_mode", he);
        }
    }

    /* Capabilities */
    cJSON *caps = cJSON_CreateArray();
    if (ups_has_cap(ctx->ups, UPS_CAP_SHUTDOWN))     cJSON_AddItemToArray(caps, cJSON_CreateString("shutdown"));
    if (ups_has_cap(ctx->ups, UPS_CAP_BATTERY_TEST)) cJSON_AddItemToArray(caps, cJSON_CreateString("battery_test"));
    if (ups_has_cap(ctx->ups, UPS_CAP_RUNTIME_CAL))  cJSON_AddItemToArray(caps, cJSON_CreateString("runtime_cal"));
    if (ups_has_cap(ctx->ups, UPS_CAP_CLEAR_FAULTS)) cJSON_AddItemToArray(caps, cJSON_CreateString("clear_faults"));
    if (ups_has_cap(ctx->ups, UPS_CAP_MUTE))         cJSON_AddItemToArray(caps, cJSON_CreateString("mute"));
    if (ups_has_cap(ctx->ups, UPS_CAP_BEEP))         cJSON_AddItemToArray(caps, cJSON_CreateString("beep"));
    if (ups_has_cap(ctx->ups, UPS_CAP_BYPASS))       cJSON_AddItemToArray(caps, cJSON_CreateString("bypass"));
    if (ups_has_cap(ctx->ups, UPS_CAP_FREQ_TOLERANCE)) cJSON_AddItemToArray(caps, cJSON_CreateString("freq_tolerance"));
    if (ups_has_cap(ctx->ups, UPS_CAP_HE_MODE))      cJSON_AddItemToArray(caps, cJSON_CreateString("he_mode"));
    cJSON_AddItemToObject(obj, "capabilities", caps);

    return obj;
}

/* --- Core route handlers --- */

static api_response_t handle_version(const api_request_t *req, void *ud)
{
    (void)req;
    (void)ud;
    cJSON *obj = cJSON_CreateObject();
#ifdef VERSION_STRING
    cJSON_AddStringToObject(obj, "daemon", VERSION_STRING);
#else
    cJSON_AddStringToObject(obj, "daemon", "unknown");
#endif
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return api_ok(json);
}

static api_response_t handle_status(const api_request_t *req, void *ud)
{
    (void)req;
    route_ctx_t *ctx = ud;
    cJSON *obj = build_status_json(ctx);
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return api_ok(json);
}

static api_response_t handle_cmd(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;

    if (!ctx->ups)
        return api_error(503, "no UPS connected");

    if (!req->body || req->body_len == 0)
        return api_error(400, "request body required");

    cJSON *body = cJSON_Parse(req->body);
    if (!body)
        return api_error(400, "invalid JSON");

    const cJSON *action = cJSON_GetObjectItem(body, "action");
    if (!action || !cJSON_IsString(action)) {
        cJSON_Delete(body);
        return api_error(400, "missing 'action' field");
    }

    const char *act = action->valuestring;
    int rc;
    const char *result_msg = "ok";

    if (strcmp(act, "shutdown_workflow") == 0) {
        int dry_run = 0;
        const cJSON *dr = cJSON_GetObjectItem(body, "dry_run");
        if (dr && cJSON_IsBool(dr)) dry_run = cJSON_IsTrue(dr);
        rc = shutdown_execute(ctx->shutdown, dry_run);
        result_msg = dry_run ? "dry run complete" : "shutdown initiated";

    } else {
        const ups_cmd_desc_t *cmd = ups_find_command(ctx->ups, act);
        if (!cmd) {
            cJSON_Delete(body);
            return api_error(400, "unknown action");
        }

        int is_off = 0;
        if (cmd->type == UPS_CMD_TOGGLE) {
            const cJSON *joff = cJSON_GetObjectItem(body, "off");
            if (joff && cJSON_IsBool(joff)) is_off = cJSON_IsTrue(joff);
        }

        rc = ups_cmd_execute(ctx->ups, act, is_off);
        result_msg = cmd->display_name;
    }

    cJSON_Delete(body);

    if (rc == UPS_ERR_NOT_SUPPORTED)
        return api_error(400, "command not supported by this UPS");
    if (rc != 0)
        return api_error(500, "command failed");

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", result_msg);
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return api_ok(json);
}

static api_response_t handle_events(const api_request_t *req, void *ud)
{
    (void)req;
    route_ctx_t *ctx = ud;

    db_result_t *result = NULL;
    int rc = db_execute(ctx->db,
        "SELECT timestamp, severity, category, title, message "
        "FROM events ORDER BY id DESC LIMIT 50",
        NULL, &result);

    if (rc != 0 || !result)
        return api_error(500, "failed to query events");

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < result->nrows; i++) {
        cJSON *ev = cJSON_CreateObject();
        cJSON_AddStringToObject(ev, "timestamp", result->rows[i][0]);
        cJSON_AddStringToObject(ev, "severity", result->rows[i][1]);
        cJSON_AddStringToObject(ev, "category", result->rows[i][2]);
        cJSON_AddStringToObject(ev, "title", result->rows[i][3]);
        cJSON_AddStringToObject(ev, "message", result->rows[i][4]);
        cJSON_AddItemToArray(arr, ev);
    }

    db_result_free(result);
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return api_ok(json);
}

static api_response_t handle_telemetry(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;

    const char *from_param = api_query_param(req, "from");
    const char *to_param = api_query_param(req, "to");
    const char *limit_param = api_query_param(req, "limit");

    int limit = limit_param ? atoi(limit_param) : 500;
    if (limit <= 0 || limit > 10000) limit = 500;

    char limit_s[16];
    snprintf(limit_s, sizeof(limit_s), "%d", limit);

    db_result_t *result = NULL;
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

    if (rc != 0 || !result)
        return api_error(500, "failed to query telemetry");

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < result->nrows; i++) {
        cJSON *pt = cJSON_CreateObject();
        cJSON_AddStringToObject(pt, "timestamp", result->rows[i][0]);
        cJSON_AddNumberToObject(pt, "status", atof(result->rows[i][1]));
        cJSON_AddNumberToObject(pt, "charge_pct", atof(result->rows[i][2]));
        cJSON_AddNumberToObject(pt, "runtime_sec", atof(result->rows[i][3]));
        cJSON_AddNumberToObject(pt, "battery_voltage", atof(result->rows[i][4]));
        cJSON_AddNumberToObject(pt, "load_pct", atof(result->rows[i][5]));
        cJSON_AddNumberToObject(pt, "output_voltage", atof(result->rows[i][6]));
        cJSON_AddNumberToObject(pt, "output_frequency", atof(result->rows[i][7]));
        cJSON_AddNumberToObject(pt, "output_current", atof(result->rows[i][8]));
        cJSON_AddNumberToObject(pt, "input_voltage", atof(result->rows[i][9]));
        cJSON_AddNumberToObject(pt, "efficiency", atof(result->rows[i][10]));
        cJSON_AddItemToArray(arr, pt);
    }

    db_result_free(result);
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return api_ok(json);
}

static api_response_t handle_commands(const api_request_t *req, void *ud)
{
    (void)req;
    route_ctx_t *ctx = ud;

    cJSON *arr = cJSON_CreateArray();
    if (ctx->ups) {
        size_t count;
        const ups_cmd_desc_t *cmds = ups_get_commands(ctx->ups, &count);
        for (size_t i = 0; i < count; i++) {
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddStringToObject(obj, "name", cmds[i].name);
            cJSON_AddStringToObject(obj, "display_name", cmds[i].display_name);
            cJSON_AddStringToObject(obj, "description", cmds[i].description);
            cJSON_AddStringToObject(obj, "group", cmds[i].group);
            cJSON_AddStringToObject(obj, "confirm_title", cmds[i].confirm_title);
            cJSON_AddStringToObject(obj, "confirm_body", cmds[i].confirm_body);
            cJSON_AddStringToObject(obj, "type",
                cmds[i].type == UPS_CMD_TOGGLE ? "toggle" : "simple");
            cJSON_AddStringToObject(obj, "variant",
                cmds[i].variant == UPS_CMD_DANGER ? "danger" :
                cmds[i].variant == UPS_CMD_WARN ? "warn" : "default");
            if (cmds[i].flags & UPS_CMD_IS_SHUTDOWN)
                cJSON_AddBoolToObject(obj, "is_shutdown", 1);
            if (cmds[i].flags & UPS_CMD_IS_MUTE)
                cJSON_AddBoolToObject(obj, "is_mute", 1);
            if (cmds[i].type == UPS_CMD_TOGGLE)
                cJSON_AddNumberToObject(obj, "status_bit", cmds[i].status_bit);
            cJSON_AddItemToArray(arr, obj);
        }
    }

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return api_ok(json);
}

/* --- Restart endpoint --- */

extern void app_request_restart(void);

static api_response_t handle_restart(const api_request_t *req, void *ud)
{
    (void)req;
    (void)ud;

    log_info("restart requested via API");
    app_request_restart();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "restarting");
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return api_ok(json);
}

/* --- Threshold cache --- */

void api_refresh_thresholds(route_ctx_t *ctx)
{
    if (!ctx->ups) return;
    ups_read_thresholds(ctx->ups, &ctx->transfer_high, &ctx->transfer_low);
}

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
