#include "api/routes.h"
#include "ups/ups.h"
#include "alerts/alerts.h"
#include <cutils/log.h>
#include <cJSON.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* --- Helper: build JSON status object --- */

static cJSON *build_status_json(route_ctx_t *ctx)
{
    cJSON *obj = cJSON_CreateObject();
    ups_data_t data;
    ups_inventory_t inv;

    cJSON_AddStringToObject(obj, "driver", monitor_driver_name(ctx->monitor));
    cJSON_AddBoolToObject(obj, "connected", monitor_is_connected(ctx->monitor));

    if (monitor_get_inventory(ctx->monitor, &inv) == 0) {
        cJSON *inv_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(inv_obj, "model", inv.model);
        cJSON_AddStringToObject(inv_obj, "serial", inv.serial);
        cJSON_AddStringToObject(inv_obj, "firmware", inv.firmware);
        cJSON_AddNumberToObject(inv_obj, "nominal_va", inv.nominal_va);
        cJSON_AddNumberToObject(inv_obj, "nominal_watts", inv.nominal_watts);
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
        cJSON_AddItemToObject(obj, "output", out);

        cJSON *in = cJSON_CreateObject();
        cJSON_AddNumberToObject(in, "voltage", data.input_voltage);
        cJSON_AddItemToObject(obj, "input", in);

        cJSON_AddNumberToObject(obj, "efficiency", data.efficiency);
        cJSON_AddStringToObject(obj, "transfer_reason",
                                ups_transfer_reason_str(data.transfer_reason));

        /* HE inhibit */
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

/* --- Route handlers --- */

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

    if (strcmp(act, "shutdown") == 0) {
        int dry_run = 0;
        const cJSON *dr = cJSON_GetObjectItem(body, "dry_run");
        if (dr && cJSON_IsBool(dr)) dry_run = cJSON_IsTrue(dr);
        rc = shutdown_execute(ctx->shutdown, dry_run, 0, 0);
        result_msg = dry_run ? "dry run complete" : "shutdown initiated";

    } else if (strcmp(act, "battery_test") == 0) {
        rc = ups_cmd_battery_test(ctx->ups);
        result_msg = "battery test started";

    } else if (strcmp(act, "runtime_cal") == 0) {
        rc = ups_cmd_runtime_cal(ctx->ups);
        result_msg = "runtime calibration started";

    } else if (strcmp(act, "clear_faults") == 0) {
        rc = ups_cmd_clear_faults(ctx->ups);
        result_msg = "faults cleared";

    } else if (strcmp(act, "mute") == 0) {
        rc = ups_cmd_mute_alarm(ctx->ups);
        result_msg = "alarm muted";

    } else if (strcmp(act, "unmute") == 0) {
        rc = ups_cmd_cancel_mute(ctx->ups);
        result_msg = "alarm unmuted";

    } else if (strcmp(act, "beep") == 0) {
        rc = ups_cmd_beep_test(ctx->ups);
        result_msg = "beep test sent";

    } else if (strcmp(act, "bypass_on") == 0) {
        rc = ups_cmd_bypass_enable(ctx->ups);
        result_msg = "bypass enabled";

    } else if (strcmp(act, "bypass_off") == 0) {
        rc = ups_cmd_bypass_disable(ctx->ups);
        result_msg = "bypass disabled";

    } else if (strcmp(act, "freq") == 0) {
        const cJSON *setting = cJSON_GetObjectItem(body, "setting");
        if (!setting || !cJSON_IsString(setting)) {
            cJSON_Delete(body);
            return api_error(400, "missing 'setting' field for freq command");
        }
        const ups_freq_setting_t *fs = ups_find_freq_setting(ctx->ups, setting->valuestring);
        if (!fs) {
            cJSON_Delete(body);
            return api_error(400, "invalid frequency setting");
        }
        rc = ups_cmd_set_freq_tolerance(ctx->ups, fs->value);
        if (rc == 0 && fs->inhibits_he) {
            const cJSON *src = cJSON_GetObjectItem(body, "source");
            monitor_he_inhibit_set(ctx->monitor,
                src && cJSON_IsString(src) ? src->valuestring : "manual");
        } else if (rc == 0) {
            monitor_he_inhibit_clear(ctx->monitor);
        }
        result_msg = "frequency tolerance set";

    } else {
        cJSON_Delete(body);
        return api_error(400, "unknown action");
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

    /* Default limit 50, configurable via ?limit=N in the future */
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
    (void)req;
    route_ctx_t *ctx = ud;

    db_result_t *result = NULL;
    int rc = db_execute(ctx->db,
        "SELECT timestamp, status, charge_pct, runtime_sec, battery_voltage, "
        "load_pct, output_voltage, output_frequency, output_current, "
        "input_voltage, efficiency "
        "FROM telemetry ORDER BY id DESC LIMIT 100",
        NULL, &result);

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

/* --- Route registration --- */

void api_register_routes(api_server_t *srv, route_ctx_t *ctx)
{
    api_server_route(srv, "/api/status",    API_GET,  handle_status,    ctx);
    api_server_route(srv, "/api/cmd",       API_POST, handle_cmd,       ctx);
    api_server_route(srv, "/api/events",    API_GET,  handle_events,    ctx);
    api_server_route(srv, "/api/telemetry", API_GET,  handle_telemetry, ctx);
}
