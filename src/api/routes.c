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
        cJSON_AddNumberToObject(inv_obj, "freq_tolerance", inv.freq_tolerance);
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

        /* Outlet group states */
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

        cJSON_AddNumberToObject(obj, "efficiency", data.efficiency);
        cJSON_AddStringToObject(obj, "transfer_reason",
                                ups_transfer_reason_str(data.transfer_reason));

        /* Error registers */
        cJSON *errors = cJSON_CreateObject();
        cJSON_AddNumberToObject(errors, "general", data.general_error);
        cJSON_AddNumberToObject(errors, "power_system", data.power_system_error);
        cJSON_AddNumberToObject(errors, "battery_system", data.bat_system_error);
        cJSON_AddItemToObject(obj, "errors", errors);

        /* Test/cal status */
        cJSON_AddNumberToObject(obj, "bat_test_status", data.bat_test_status);
        cJSON_AddNumberToObject(obj, "rt_cal_status", data.rt_cal_status);
        cJSON_AddNumberToObject(obj, "bat_lifetime_status", data.bat_lifetime_status);

        /* Timers */
        cJSON *timers = cJSON_CreateObject();
        cJSON_AddNumberToObject(timers, "shutdown", data.timer_shutdown);
        cJSON_AddNumberToObject(timers, "start", data.timer_start);
        cJSON_AddNumberToObject(timers, "reboot", data.timer_reboot);
        cJSON_AddItemToObject(obj, "timers", timers);

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

    } else if (strcmp(act, "beep_short") == 0) {
        rc = ups_cmd_beep_short(ctx->ups);
        result_msg = "short beep test sent";

    } else if (strcmp(act, "beep_continuous") == 0) {
        rc = ups_cmd_beep_continuous(ctx->ups);
        result_msg = "continuous test started";

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
    route_ctx_t *ctx = ud;

    /* Query params: ?from=YYYY-MM-DD&to=YYYY-MM-DD&limit=N */
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

/* --- UPS config register endpoints --- */

static cJSON *reg_to_json(const ups_config_reg_t *reg, uint16_t raw,
                          const char *str_val)
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

    /* Add human-readable date for date fields */
    if (reg->unit && strcmp(reg->unit, "days since 2000-01-01") == 0 && raw > 0) {
        time_t epoch = 946684800 + (time_t)raw * 86400; /* 2000-01-01 UTC */
        struct tm tm;
        gmtime_r(&epoch, &tm);
        char datebuf[16];
        strftime(datebuf, sizeof(datebuf), "%Y-%m-%d", &tm);
        cJSON_AddStringToObject(obj, "date", datebuf);
    }

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
        if (str_val)
            cJSON_AddStringToObject(obj, "string_value", str_val);
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
        char str_buf[64] = "";
        if (ups_config_read(ctx->ups, reg, &raw,
                            reg->type == UPS_CFG_STRING ? str_buf : NULL,
                            sizeof(str_buf)) != 0)
            return api_error(500, "failed to read register");

        cJSON *obj = reg_to_json(reg, raw,
                                 reg->type == UPS_CFG_STRING ? str_buf : NULL);
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
        char str_buf[64] = "";
        if (ups_config_read(ctx->ups, &regs[i], &raw,
                            regs[i].type == UPS_CFG_STRING ? str_buf : NULL,
                            sizeof(str_buf)) == 0)
            cJSON_AddItemToArray(arr, reg_to_json(&regs[i], raw,
                regs[i].type == UPS_CFG_STRING ? str_buf : NULL));
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

    /* Always read back current value regardless of write result */
    uint16_t readback = 0;
    ups_config_read(ctx->ups, reg, &readback, NULL, 0);

    cJSON *resp = reg_to_json(reg, readback, NULL);
    if (rc != 0) {
        if (readback != val)
            cJSON_AddStringToObject(resp, "result", "rejected");
        else
            cJSON_AddStringToObject(resp, "result", "written");
    } else {
        cJSON_AddStringToObject(resp, "result", "written");
    }
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);

    /* Refresh cached thresholds if a transfer register was written */
    if (strcmp(reg->name, "transfer_high") == 0 ||
        strcmp(reg->name, "transfer_low") == 0)
        api_refresh_thresholds(ctx);

    /* Return 200 even on rejection so the UI gets the current value */
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
        return api_error(401, "invalid password");
    }
    cJSON_Delete(body);

    char *token = auth_create_token(ctx->db, 24);
    if (!token) return api_error(500, "failed to create session");

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "token", token);
    cJSON_AddNumberToObject(resp, "expires_in", 86400);
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    free(token);
    return api_ok(json);
}

/* --- Weather endpoints --- */

static api_response_t handle_weather_status(const api_request_t *req, void *ud)
{
    (void)req;
    route_ctx_t *ctx = ud;
    char *json = weather_status_json(ctx->weather);
    if (!json) return api_error(500, "weather status unavailable");
    return api_ok(json);
}

static api_response_t handle_weather_config_get(const api_request_t *req, void *ud)
{
    (void)req;
    route_ctx_t *ctx = ud;
    db_result_t *result = NULL;
    int rc = db_execute(ctx->db,
        "SELECT enabled, latitude, longitude, alert_zones, alert_types, "
        "wind_speed_mph, severe_keywords, poll_interval, "
        "control_register, severe_raw_value, normal_raw_value "
        "FROM weather_config WHERE id = 1",
        NULL, &result);

    if (rc != 0 || !result || result->nrows == 0) {
        db_result_free(result);
        return api_error(404, "weather not configured");
    }

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "enabled", atoi(result->rows[0][0]));
    cJSON_AddNumberToObject(obj, "latitude", atof(result->rows[0][1]));
    cJSON_AddNumberToObject(obj, "longitude", atof(result->rows[0][2]));
    cJSON_AddStringToObject(obj, "alert_zones", result->rows[0][3] ? result->rows[0][3] : "");
    cJSON_AddStringToObject(obj, "alert_types", result->rows[0][4] ? result->rows[0][4] : "");
    cJSON_AddNumberToObject(obj, "wind_speed_mph", atoi(result->rows[0][5]));
    cJSON_AddStringToObject(obj, "severe_keywords", result->rows[0][6] ? result->rows[0][6] : "");
    cJSON_AddNumberToObject(obj, "poll_interval", atoi(result->rows[0][7]));
    cJSON_AddStringToObject(obj, "control_register", result->rows[0][8] ? result->rows[0][8] : "freq_tolerance");
    if (result->rows[0][9])
        cJSON_AddNumberToObject(obj, "severe_raw_value", atoi(result->rows[0][9]));
    if (result->rows[0][10])
        cJSON_AddNumberToObject(obj, "normal_raw_value", atoi(result->rows[0][10]));

    db_result_free(result);
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return api_ok(json);
}

static api_response_t handle_weather_config_set(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;
    if (!req->body) return api_error(400, "request body required");
    cJSON *body = cJSON_Parse(req->body);
    if (!body) return api_error(400, "invalid JSON");

    /* Update only fields that are present in the request */
    const cJSON *jen  = cJSON_GetObjectItem(body, "enabled");
    const cJSON *jlat = cJSON_GetObjectItem(body, "latitude");
    const cJSON *jlon = cJSON_GetObjectItem(body, "longitude");
    const cJSON *jzn  = cJSON_GetObjectItem(body, "alert_zones");
    const cJSON *jat  = cJSON_GetObjectItem(body, "alert_types");
    const cJSON *jws  = cJSON_GetObjectItem(body, "wind_speed_mph");
    const cJSON *jsk  = cJSON_GetObjectItem(body, "severe_keywords");
    const cJSON *jpi  = cJSON_GetObjectItem(body, "poll_interval");
    const cJSON *jcr  = cJSON_GetObjectItem(body, "control_register");
    const cJSON *jsrv = cJSON_GetObjectItem(body, "severe_raw_value");
    const cJSON *jnrv = cJSON_GetObjectItem(body, "normal_raw_value");

    char en_s[4], lat_s[32], lon_s[32], ws_s[16], pi_s[16], srv_s[16], nrv_s[16];

    /* Read current values as defaults */
    db_result_t *cur = NULL;
    db_execute(ctx->db,
        "SELECT enabled, latitude, longitude, alert_zones, alert_types, "
        "wind_speed_mph, severe_keywords, poll_interval, "
        "control_register, severe_raw_value, normal_raw_value "
        "FROM weather_config WHERE id = 1", NULL, &cur);

    if (!cur || cur->nrows == 0) {
        db_result_free(cur);
        cJSON_Delete(body);
        return api_error(404, "weather config not found");
    }

    snprintf(en_s, sizeof(en_s), "%d",
             jen && cJSON_IsBool(jen) ? cJSON_IsTrue(jen) : atoi(cur->rows[0][0]));
    snprintf(lat_s, sizeof(lat_s), "%.6f",
             jlat && cJSON_IsNumber(jlat) ? jlat->valuedouble : atof(cur->rows[0][1]));
    snprintf(lon_s, sizeof(lon_s), "%.6f",
             jlon && cJSON_IsNumber(jlon) ? jlon->valuedouble : atof(cur->rows[0][2]));
    const char *zones = jzn && cJSON_IsString(jzn) ? jzn->valuestring : cur->rows[0][3];
    const char *atypes = jat && cJSON_IsString(jat) ? jat->valuestring : cur->rows[0][4];
    snprintf(ws_s, sizeof(ws_s), "%d",
             jws && cJSON_IsNumber(jws) ? jws->valueint : atoi(cur->rows[0][5]));
    const char *skw = jsk && cJSON_IsString(jsk) ? jsk->valuestring : cur->rows[0][6];
    snprintf(pi_s, sizeof(pi_s), "%d",
             jpi && cJSON_IsNumber(jpi) ? jpi->valueint : atoi(cur->rows[0][7]));
    const char *creg = jcr && cJSON_IsString(jcr) ? jcr->valuestring : cur->rows[0][8];
    snprintf(srv_s, sizeof(srv_s), "%d",
             jsrv && cJSON_IsNumber(jsrv) ? jsrv->valueint :
             (cur->rows[0][9] ? atoi(cur->rows[0][9]) : 0));
    snprintf(nrv_s, sizeof(nrv_s), "%d",
             jnrv && cJSON_IsNumber(jnrv) ? jnrv->valueint :
             (cur->rows[0][10] ? atoi(cur->rows[0][10]) : 0));

    const char *params[] = {
        en_s, lat_s, lon_s,
        zones ? zones : "", atypes ? atypes : "",
        ws_s, skw ? skw : "", pi_s,
        creg ? creg : "freq_tolerance", srv_s, nrv_s,
        NULL
    };
    int rc = db_execute_non_query(ctx->db,
        "UPDATE weather_config SET enabled=?, latitude=?, longitude=?, "
        "alert_zones=?, alert_types=?, wind_speed_mph=?, "
        "severe_keywords=?, poll_interval=?, "
        "control_register=?, severe_raw_value=?, normal_raw_value=? WHERE id = 1",
        params, NULL);

    db_result_free(cur);
    cJSON_Delete(body);

    if (rc != 0) return api_error(500, "failed to update weather config");

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "updated");
    cJSON_AddStringToObject(resp, "note", "restart daemon to apply changes");
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return api_ok(json);
}

/* --- Restart endpoint --- */

/* Defined in daemon/main.c — sets flag for main loop */
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
    /* Populate threshold cache at startup */
    api_refresh_thresholds(ctx);
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
    api_server_route(srv, "/api/restart",      API_POST, handle_restart,        ctx);
    api_server_route(srv, "/api/auth/setup",   API_POST, handle_auth_setup, ctx);
    api_server_route(srv, "/api/auth/login",   API_POST, handle_auth_login, ctx);
    api_server_route(srv, "/api/weather/status", API_GET,  handle_weather_status,     ctx);
    api_server_route(srv, "/api/weather/config", API_GET,  handle_weather_config_get, ctx);
    api_server_route(srv, "/api/weather/config", API_POST, handle_weather_config_set, ctx);
}
