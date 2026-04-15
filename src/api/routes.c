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

    if (!ctx->monitor || !ctx->ups) {
        cJSON_AddStringToObject(obj, "driver", "none");
        cJSON_AddBoolToObject(obj, "connected", 0);
        cJSON_AddStringToObject(obj, "message", "no UPS connected");
        return obj;
    }

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

/* --- UPS config register endpoints --- */

static cJSON *reg_to_json(const ups_config_reg_t *reg, uint16_t raw)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "name", reg->name);
    cJSON_AddStringToObject(obj, "display_name", reg->display_name);
    if (reg->unit) cJSON_AddStringToObject(obj, "unit", reg->unit);
    if (reg->group) cJSON_AddStringToObject(obj, "group", reg->group);
    cJSON_AddNumberToObject(obj, "raw_value", raw);
    cJSON_AddBoolToObject(obj, "writable", reg->writable);

    double scaled = (reg->scale > 1) ? (double)raw / reg->scale : (double)raw;
    cJSON_AddNumberToObject(obj, "value", scaled);

    if (reg->type == UPS_CFG_SCALAR) {
        cJSON_AddStringToObject(obj, "type", "scalar");
    } else if (reg->type == UPS_CFG_BITFIELD) {
        cJSON_AddStringToObject(obj, "type", "bitfield");
        /* Find matching option name */
        for (size_t i = 0; i < reg->meta.bitfield.count; i++) {
            if (reg->meta.bitfield.opts[i].value == raw) {
                cJSON_AddStringToObject(obj, "setting", reg->meta.bitfield.opts[i].name);
                cJSON_AddStringToObject(obj, "setting_label", reg->meta.bitfield.opts[i].label);
                break;
            }
        }
        /* Add all options */
        cJSON *opts = cJSON_CreateArray();
        for (size_t i = 0; i < reg->meta.bitfield.count; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "value", reg->meta.bitfield.opts[i].value);
            cJSON_AddStringToObject(o, "name", reg->meta.bitfield.opts[i].name);
            cJSON_AddStringToObject(o, "label", reg->meta.bitfield.opts[i].label);
            cJSON_AddItemToArray(opts, o);
        }
        cJSON_AddItemToObject(obj, "options", opts);
    } else if (reg->type == UPS_CFG_STRING) {
        cJSON_AddStringToObject(obj, "type", "string");
    }

    return obj;
}

static api_response_t handle_config_ups_get(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;
    if (!ctx->ups) return api_error(503, "no UPS connected");

    /* Check if a specific register is requested: /api/config/ups/<name> */
    const char *name = NULL;
    if (strlen(req->url) > 16)  /* longer than "/api/config/ups/" */
        name = req->url + 16;

    if (name && name[0]) {
        /* Single register */
        const ups_config_reg_t *reg = ups_find_config_reg(ctx->ups, name);
        if (!reg) return api_error(404, "register not found");

        uint16_t raw = 0;
        if (ups_config_read(ctx->ups, reg, &raw, NULL, 0) != 0)
            return api_error(500, "failed to read register");

        cJSON *obj = reg_to_json(reg, raw);
        char *json = cJSON_PrintUnformatted(obj);
        cJSON_Delete(obj);
        return api_ok(json);
    }

    /* Dump all registers */
    size_t count;
    const ups_config_reg_t *regs = ups_get_config_regs(ctx->ups, &count);
    if (!regs || count == 0)
        return api_ok(strdup("[]"));

    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        uint16_t raw = 0;
        if (ups_config_read(ctx->ups, &regs[i], &raw, NULL, 0) == 0)
            cJSON_AddItemToArray(arr, reg_to_json(&regs[i], raw));
    }

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return api_ok(json);
}

static api_response_t handle_config_ups_set(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;
    if (!ctx->ups) return api_error(503, "no UPS connected");
    if (!req->body) return api_error(400, "request body required");

    cJSON *body = cJSON_Parse(req->body);
    if (!body) return api_error(400, "invalid JSON");

    const cJSON *jname = cJSON_GetObjectItem(body, "name");
    const cJSON *jval  = cJSON_GetObjectItem(body, "value");
    if (!jname || !cJSON_IsString(jname) || !jval || !cJSON_IsNumber(jval)) {
        cJSON_Delete(body);
        return api_error(400, "missing 'name' (string) and 'value' (number)");
    }

    const ups_config_reg_t *reg = ups_find_config_reg(ctx->ups, jname->valuestring);
    if (!reg) { cJSON_Delete(body); return api_error(404, "register not found"); }
    if (!reg->writable) { cJSON_Delete(body); return api_error(400, "register is read-only"); }

    uint16_t val = (uint16_t)jval->valueint;
    cJSON_Delete(body);

    int rc = ups_config_write(ctx->ups, reg, val);
    if (rc != 0) return api_error(500, "failed to write register");

    /* Read back and return updated value */
    uint16_t readback = 0;
    ups_config_read(ctx->ups, reg, &readback, NULL, 0);

    cJSON *resp = reg_to_json(reg, readback);
    cJSON_AddStringToObject(resp, "result", "written");
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return api_ok(json);
}

/* --- Shutdown target/group CRUD --- */

static api_response_t handle_shutdown_groups_get(const api_request_t *req, void *ud)
{
    (void)req;
    route_ctx_t *ctx = ud;
    db_result_t *result = NULL;
    int rc = db_execute(ctx->db,
        "SELECT id, name, execution_order, parallel FROM shutdown_groups ORDER BY execution_order",
        NULL, &result);
    if (rc != 0 || !result) return api_error(500, "failed to query groups");

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < result->nrows; i++) {
        cJSON *g = cJSON_CreateObject();
        cJSON_AddNumberToObject(g, "id", atoi(result->rows[i][0]));
        cJSON_AddStringToObject(g, "name", result->rows[i][1]);
        cJSON_AddNumberToObject(g, "execution_order", atoi(result->rows[i][2]));
        cJSON_AddBoolToObject(g, "parallel", atoi(result->rows[i][3]));
        cJSON_AddItemToArray(arr, g);
    }
    db_result_free(result);
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return api_ok(json);
}

static api_response_t handle_shutdown_groups_post(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;
    if (!req->body) return api_error(400, "request body required");
    cJSON *body = cJSON_Parse(req->body);
    if (!body) return api_error(400, "invalid JSON");

    const cJSON *jname  = cJSON_GetObjectItem(body, "name");
    const cJSON *jorder = cJSON_GetObjectItem(body, "execution_order");
    const cJSON *jpar   = cJSON_GetObjectItem(body, "parallel");

    if (!jname || !cJSON_IsString(jname)) {
        cJSON_Delete(body); return api_error(400, "missing 'name'");
    }

    char order_s[16] = "0", par_s[4] = "1";
    if (jorder && cJSON_IsNumber(jorder)) snprintf(order_s, sizeof(order_s), "%d", jorder->valueint);
    if (jpar && cJSON_IsBool(jpar)) snprintf(par_s, sizeof(par_s), "%d", cJSON_IsTrue(jpar));

    const char *params[] = { jname->valuestring, order_s, par_s, NULL };
    int rc = db_execute_non_query(ctx->db,
        "INSERT INTO shutdown_groups (name, execution_order, parallel) VALUES (?, ?, ?)",
        params, NULL);
    cJSON_Delete(body);
    if (rc != 0) return api_error(500, "failed to create group");

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "created");
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return api_ok_status(201, json);
}

static api_response_t handle_shutdown_targets_get(const api_request_t *req, void *ud)
{
    (void)req;
    route_ctx_t *ctx = ud;
    db_result_t *result = NULL;
    int rc = db_execute(ctx->db,
        "SELECT t.id, t.name, t.method, t.host, t.username, t.command, "
        "t.timeout_sec, t.order_in_group, g.name AS group_name "
        "FROM shutdown_targets t JOIN shutdown_groups g ON t.group_id = g.id "
        "ORDER BY g.execution_order, t.order_in_group",
        NULL, &result);
    if (rc != 0 || !result) return api_error(500, "failed to query targets");

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < result->nrows; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddNumberToObject(t, "id", atoi(result->rows[i][0]));
        cJSON_AddStringToObject(t, "name", result->rows[i][1]);
        cJSON_AddStringToObject(t, "method", result->rows[i][2]);
        cJSON_AddStringToObject(t, "host", result->rows[i][3]);
        cJSON_AddStringToObject(t, "username", result->rows[i][4]);
        cJSON_AddStringToObject(t, "command", result->rows[i][5]);
        cJSON_AddNumberToObject(t, "timeout_sec", atoi(result->rows[i][6]));
        cJSON_AddNumberToObject(t, "order_in_group", atoi(result->rows[i][7]));
        cJSON_AddStringToObject(t, "group", result->rows[i][8]);
        cJSON_AddItemToArray(arr, t);
    }
    db_result_free(result);
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return api_ok(json);
}

static api_response_t handle_shutdown_targets_post(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;
    if (!req->body) return api_error(400, "request body required");
    cJSON *body = cJSON_Parse(req->body);
    if (!body) return api_error(400, "invalid JSON");

    const cJSON *jgid  = cJSON_GetObjectItem(body, "group_id");
    const cJSON *jname = cJSON_GetObjectItem(body, "name");
    const cJSON *jmeth = cJSON_GetObjectItem(body, "method");
    const cJSON *jhost = cJSON_GetObjectItem(body, "host");
    const cJSON *juser = cJSON_GetObjectItem(body, "username");
    const cJSON *jcred = cJSON_GetObjectItem(body, "credential");
    const cJSON *jcmd  = cJSON_GetObjectItem(body, "command");
    const cJSON *jtmo  = cJSON_GetObjectItem(body, "timeout_sec");

    if (!jgid || !cJSON_IsNumber(jgid) ||
        !jname || !cJSON_IsString(jname) ||
        !jcmd || !cJSON_IsString(jcmd)) {
        cJSON_Delete(body);
        return api_error(400, "missing required fields: group_id, name, command");
    }

    char gid_s[16], tmo_s[16] = "180", order_s[16] = "0";
    snprintf(gid_s, sizeof(gid_s), "%d", jgid->valueint);
    if (jtmo && cJSON_IsNumber(jtmo)) snprintf(tmo_s, sizeof(tmo_s), "%d", jtmo->valueint);

    const char *params[] = {
        gid_s,
        jname->valuestring,
        jmeth && cJSON_IsString(jmeth) ? jmeth->valuestring : "ssh_password",
        jhost && cJSON_IsString(jhost) ? jhost->valuestring : "",
        juser && cJSON_IsString(juser) ? juser->valuestring : "",
        jcred && cJSON_IsString(jcred) ? jcred->valuestring : "",
        jcmd->valuestring,
        tmo_s,
        order_s,
        NULL
    };
    int rc = db_execute_non_query(ctx->db,
        "INSERT INTO shutdown_targets (group_id, name, method, host, username, "
        "credential, command, timeout_sec, order_in_group) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
        params, NULL);
    cJSON_Delete(body);
    if (rc != 0) return api_error(500, "failed to create target");

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "created");
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return api_ok_status(201, json);
}

/* --- App config CRUD --- */

static api_response_t handle_app_config_get(const api_request_t *req, void *ud)
{
    (void)req;
    route_ctx_t *ctx = ud;
    db_result_t *result = NULL;
    int rc = db_execute(ctx->db,
        "SELECT key, value, type, default_value, description FROM config ORDER BY key",
        NULL, &result);
    if (rc != 0 || !result) return api_error(500, "failed to query config");

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < result->nrows; i++) {
        cJSON *c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "key", result->rows[i][0]);
        cJSON_AddStringToObject(c, "value", result->rows[i][1]);
        cJSON_AddStringToObject(c, "type", result->rows[i][2]);
        cJSON_AddStringToObject(c, "default_value", result->rows[i][3]);
        cJSON_AddStringToObject(c, "description", result->rows[i][4]);
        cJSON_AddItemToArray(arr, c);
    }
    db_result_free(result);
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return api_ok(json);
}

static api_response_t handle_app_config_set(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;
    if (!req->body) return api_error(400, "request body required");
    cJSON *body = cJSON_Parse(req->body);
    if (!body) return api_error(400, "invalid JSON");

    const cJSON *jkey = cJSON_GetObjectItem(body, "key");
    const cJSON *jval = cJSON_GetObjectItem(body, "value");
    if (!jkey || !cJSON_IsString(jkey) || !jval || !cJSON_IsString(jval)) {
        cJSON_Delete(body);
        return api_error(400, "missing 'key' and 'value' strings");
    }

    int rc = config_set_db(ctx->config, jkey->valuestring, jval->valuestring);
    cJSON_Delete(body);

    if (rc != 0) return api_error(400, "failed to update config");

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "updated");
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return api_ok(json);
}

/* --- Route registration --- */

void api_register_routes(api_server_t *srv, route_ctx_t *ctx)
{
    api_server_route(srv, "/api/status",       API_GET,  handle_status,         ctx);
    api_server_route(srv, "/api/cmd",          API_POST, handle_cmd,            ctx);
    api_server_route(srv, "/api/events",       API_GET,  handle_events,         ctx);
    api_server_route(srv, "/api/telemetry",    API_GET,  handle_telemetry,      ctx);
    api_server_route(srv, "/api/config/ups*",  API_GET,  handle_config_ups_get, ctx);
    api_server_route(srv, "/api/config/ups",   API_POST, handle_config_ups_set, ctx);
    api_server_route(srv, "/api/shutdown/groups",  API_GET,  handle_shutdown_groups_get,  ctx);
    api_server_route(srv, "/api/shutdown/groups",  API_POST, handle_shutdown_groups_post, ctx);
    api_server_route(srv, "/api/shutdown/targets", API_GET,  handle_shutdown_targets_get, ctx);
    api_server_route(srv, "/api/shutdown/targets", API_POST, handle_shutdown_targets_post, ctx);
    api_server_route(srv, "/api/config/app",   API_GET,  handle_app_config_get, ctx);
    api_server_route(srv, "/api/config/app",   API_POST, handle_app_config_set, ctx);
}
