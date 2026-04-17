#include "api/routes.h"
#include "ups/ups.h"
#include "alerts/alerts.h"
#include <cutils/log.h>
#include <cJSON.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>

/* --- Helpers --- */

static api_response_t api_ok_msg(const char *msg)
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

        /* Error registers — raw values + decoded string arrays */
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

    /* Special case: shutdown workflow (app-level orchestration, not a UPS command) */
    if (strcmp(act, "shutdown_workflow") == 0) {
        int dry_run = 0;
        const cJSON *dr = cJSON_GetObjectItem(body, "dry_run");
        if (dr && cJSON_IsBool(dr)) dry_run = cJSON_IsTrue(dr);
        rc = shutdown_execute(ctx->shutdown, dry_run);
        result_msg = dry_run ? "dry run complete" : "shutdown initiated";

    } else {
        /* Descriptor-driven dispatch: look up command by name */
        const ups_cmd_desc_t *cmd = ups_find_command(ctx->ups, act);
        if (!cmd) {
            cJSON_Delete(body);
            return api_error(400, "unknown action");
        }

        /* For toggles, check if this is the "off" action (name_off convention) */
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
        if (reg->meta.scalar.min != 0 || reg->meta.scalar.max != 0) {
            cJSON_AddNumberToObject(obj, "min", reg->meta.scalar.min);
            cJSON_AddNumberToObject(obj, "max", reg->meta.scalar.max);
        }
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

    /* Snapshot the new value to ups_config table */
    {
        char ts[32], raw_s[16], display_s[64];
        time_t now = time(NULL);
        struct tm tm;
        gmtime_r(&now, &tm);
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);
        snprintf(raw_s, sizeof(raw_s), "%u", readback);
        if (reg->scale > 1)
            snprintf(display_s, sizeof(display_s), "%.1f%s%s",
                     (double)readback / reg->scale,
                     reg->unit ? " " : "", reg->unit ? reg->unit : "");
        else
            snprintf(display_s, sizeof(display_s), "%u%s%s",
                     readback,
                     reg->unit ? " " : "", reg->unit ? reg->unit : "");
        const char *snap_params[] = { reg->name, raw_s, display_s, ts, NULL };
        db_execute_non_query(ctx->db,
            "INSERT INTO ups_config (register_name, raw_value, display_value, timestamp) "
            "VALUES (?, ?, ?, ?)",
            snap_params, NULL);
    }

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
        "SELECT id, name, execution_order, parallel, max_timeout_sec, post_group_delay "
        "FROM shutdown_groups ORDER BY execution_order",
        NULL, &result);
    if (rc != 0 || !result) return api_error(500, "failed to query groups");

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < result->nrows; i++) {
        cJSON *g = cJSON_CreateObject();
        cJSON_AddNumberToObject(g, "id", atoi(result->rows[i][0]));
        cJSON_AddStringToObject(g, "name", result->rows[i][1]);
        cJSON_AddNumberToObject(g, "execution_order", atoi(result->rows[i][2]));
        cJSON_AddBoolToObject(g, "parallel", atoi(result->rows[i][3]));
        cJSON_AddNumberToObject(g, "max_timeout_sec", atoi(result->rows[i][4]));
        cJSON_AddNumberToObject(g, "post_group_delay", atoi(result->rows[i][5]));
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
    const cJSON *jmtmo  = cJSON_GetObjectItem(body, "max_timeout_sec");
    const cJSON *jpgd   = cJSON_GetObjectItem(body, "post_group_delay");

    if (!jname || !cJSON_IsString(jname)) {
        cJSON_Delete(body); return api_error(400, "missing 'name'");
    }

    char order_s[16] = "0", par_s[4] = "1", mtmo_s[16] = "0", pgd_s[16] = "0";
    if (jorder && cJSON_IsNumber(jorder)) snprintf(order_s, sizeof(order_s), "%d", jorder->valueint);
    if (jpar && cJSON_IsBool(jpar)) snprintf(par_s, sizeof(par_s), "%d", cJSON_IsTrue(jpar));
    if (jmtmo && cJSON_IsNumber(jmtmo)) snprintf(mtmo_s, sizeof(mtmo_s), "%d", jmtmo->valueint);
    if (jpgd && cJSON_IsNumber(jpgd)) snprintf(pgd_s, sizeof(pgd_s), "%d", jpgd->valueint);

    const char *params[] = { jname->valuestring, order_s, par_s, mtmo_s, pgd_s, NULL };
    int rc = db_execute_non_query(ctx->db,
        "INSERT INTO shutdown_groups (name, execution_order, parallel, "
        "max_timeout_sec, post_group_delay) VALUES (?, ?, ?, ?, ?)",
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
        "t.timeout_sec, t.order_in_group, g.name AS group_name, "
        "t.confirm_method, t.confirm_port, t.confirm_command, t.post_confirm_delay, "
        "t.group_id "
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
        cJSON_AddStringToObject(t, "host", result->rows[i][3] ? result->rows[i][3] : "");
        cJSON_AddStringToObject(t, "username", result->rows[i][4] ? result->rows[i][4] : "");
        cJSON_AddStringToObject(t, "command", result->rows[i][5]);
        cJSON_AddNumberToObject(t, "timeout_sec", atoi(result->rows[i][6]));
        cJSON_AddNumberToObject(t, "order_in_group", atoi(result->rows[i][7]));
        cJSON_AddStringToObject(t, "group", result->rows[i][8]);
        cJSON_AddStringToObject(t, "confirm_method", result->rows[i][9] ? result->rows[i][9] : "ping");
        cJSON_AddNumberToObject(t, "confirm_port", result->rows[i][10] ? atoi(result->rows[i][10]) : 0);
        cJSON_AddStringToObject(t, "confirm_command", result->rows[i][11] ? result->rows[i][11] : "");
        cJSON_AddNumberToObject(t, "post_confirm_delay", atoi(result->rows[i][12]));
        cJSON_AddNumberToObject(t, "group_id", atoi(result->rows[i][13]));
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
    const cJSON *jcm   = cJSON_GetObjectItem(body, "confirm_method");
    const cJSON *jcp   = cJSON_GetObjectItem(body, "confirm_port");
    const cJSON *jcc   = cJSON_GetObjectItem(body, "confirm_command");
    const cJSON *jpcd  = cJSON_GetObjectItem(body, "post_confirm_delay");

    if (!jgid || !cJSON_IsNumber(jgid) ||
        !jname || !cJSON_IsString(jname) ||
        !jcmd || !cJSON_IsString(jcmd)) {
        cJSON_Delete(body);
        return api_error(400, "missing required fields: group_id, name, command");
    }

    char gid_s[16], tmo_s[16] = "180", order_s[16] = "0";
    char cp_s[16] = "", pcd_s[16] = "15";
    snprintf(gid_s, sizeof(gid_s), "%d", jgid->valueint);
    if (jtmo && cJSON_IsNumber(jtmo)) snprintf(tmo_s, sizeof(tmo_s), "%d", jtmo->valueint);
    if (jcp && cJSON_IsNumber(jcp)) snprintf(cp_s, sizeof(cp_s), "%d", jcp->valueint);
    if (jpcd && cJSON_IsNumber(jpcd)) snprintf(pcd_s, sizeof(pcd_s), "%d", jpcd->valueint);

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
        jcm && cJSON_IsString(jcm) ? jcm->valuestring : "ping",
        cp_s[0] ? cp_s : "",
        jcc && cJSON_IsString(jcc) ? jcc->valuestring : "",
        pcd_s,
        NULL
    };
    int rc = db_execute_non_query(ctx->db,
        "INSERT INTO shutdown_targets (group_id, name, method, host, username, "
        "credential, command, timeout_sec, order_in_group, "
        "confirm_method, confirm_port, confirm_command, post_confirm_delay) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        params, NULL);
    cJSON_Delete(body);
    if (rc != 0) return api_error(500, "failed to create target");

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "created");
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return api_ok_status(201, json);
}

/* --- Shutdown PUT/DELETE + settings --- */

static api_response_t handle_shutdown_groups_put(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;
    if (!req->body) return api_error(400, "request body required");
    cJSON *body = cJSON_Parse(req->body);
    if (!body) return api_error(400, "invalid JSON");

    const cJSON *jid = cJSON_GetObjectItem(body, "id");
    if (!jid || !cJSON_IsNumber(jid)) { cJSON_Delete(body); return api_error(400, "missing 'id'"); }

    const cJSON *jname  = cJSON_GetObjectItem(body, "name");
    const cJSON *jorder = cJSON_GetObjectItem(body, "execution_order");
    const cJSON *jpar   = cJSON_GetObjectItem(body, "parallel");
    const cJSON *jmtmo  = cJSON_GetObjectItem(body, "max_timeout_sec");
    const cJSON *jpgd   = cJSON_GetObjectItem(body, "post_group_delay");

    char id_s[16], order_s[16], par_s[4], mtmo_s[16], pgd_s[16];
    snprintf(id_s, sizeof(id_s), "%d", jid->valueint);

    /* Build SET clause dynamically would be complex; just require all fields */
    if (!jname || !cJSON_IsString(jname)) { cJSON_Delete(body); return api_error(400, "missing 'name'"); }
    snprintf(order_s, sizeof(order_s), "%d", jorder && cJSON_IsNumber(jorder) ? jorder->valueint : 0);
    snprintf(par_s, sizeof(par_s), "%d", jpar && cJSON_IsBool(jpar) ? cJSON_IsTrue(jpar) : 1);
    snprintf(mtmo_s, sizeof(mtmo_s), "%d", jmtmo && cJSON_IsNumber(jmtmo) ? jmtmo->valueint : 0);
    snprintf(pgd_s, sizeof(pgd_s), "%d", jpgd && cJSON_IsNumber(jpgd) ? jpgd->valueint : 0);

    const char *params[] = { jname->valuestring, order_s, par_s, mtmo_s, pgd_s, id_s, NULL };
    int rc = db_execute_non_query(ctx->db,
        "UPDATE shutdown_groups SET name=?, execution_order=?, parallel=?, "
        "max_timeout_sec=?, post_group_delay=? WHERE id=?",
        params, NULL);
    cJSON_Delete(body);
    if (rc != 0) return api_error(500, "failed to update group");
    return api_ok_msg("updated");
}

static api_response_t handle_shutdown_groups_delete(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;
    if (!req->body) return api_error(400, "request body required");
    cJSON *body = cJSON_Parse(req->body);
    if (!body) return api_error(400, "invalid JSON");

    const cJSON *jid = cJSON_GetObjectItem(body, "id");
    if (!jid || !cJSON_IsNumber(jid)) { cJSON_Delete(body); return api_error(400, "missing 'id'"); }

    char id_s[16];
    snprintf(id_s, sizeof(id_s), "%d", jid->valueint);

    /* CASCADE on FK deletes targets automatically */
    const char *params[] = { id_s, NULL };
    int rc = db_execute_non_query(ctx->db,
        "DELETE FROM shutdown_groups WHERE id=?", params, NULL);
    cJSON_Delete(body);
    if (rc != 0) return api_error(500, "failed to delete group");
    return api_ok_msg("deleted");
}

static api_response_t handle_shutdown_targets_put(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;
    if (!req->body) return api_error(400, "request body required");
    cJSON *body = cJSON_Parse(req->body);
    if (!body) return api_error(400, "invalid JSON");

    const cJSON *jid   = cJSON_GetObjectItem(body, "id");
    const cJSON *jgid  = cJSON_GetObjectItem(body, "group_id");
    const cJSON *jname = cJSON_GetObjectItem(body, "name");
    const cJSON *jmeth = cJSON_GetObjectItem(body, "method");
    const cJSON *jhost = cJSON_GetObjectItem(body, "host");
    const cJSON *juser = cJSON_GetObjectItem(body, "username");
    const cJSON *jcred = cJSON_GetObjectItem(body, "credential");
    const cJSON *jcmd  = cJSON_GetObjectItem(body, "command");
    const cJSON *jtmo  = cJSON_GetObjectItem(body, "timeout_sec");
    const cJSON *jord  = cJSON_GetObjectItem(body, "order_in_group");
    const cJSON *jcm   = cJSON_GetObjectItem(body, "confirm_method");
    const cJSON *jcp   = cJSON_GetObjectItem(body, "confirm_port");
    const cJSON *jcc   = cJSON_GetObjectItem(body, "confirm_command");
    const cJSON *jpcd  = cJSON_GetObjectItem(body, "post_confirm_delay");

    if (!jid || !cJSON_IsNumber(jid) ||
        !jname || !cJSON_IsString(jname) ||
        !jcmd || !cJSON_IsString(jcmd)) {
        cJSON_Delete(body);
        return api_error(400, "missing required fields: id, name, command");
    }

    char id_s[16], gid_s[16], tmo_s[16], ord_s[16], cp_s[16], pcd_s[16];
    snprintf(id_s, sizeof(id_s), "%d", jid->valueint);
    snprintf(gid_s, sizeof(gid_s), "%d", jgid && cJSON_IsNumber(jgid) ? jgid->valueint : 0);
    snprintf(tmo_s, sizeof(tmo_s), "%d", jtmo && cJSON_IsNumber(jtmo) ? jtmo->valueint : 180);
    snprintf(ord_s, sizeof(ord_s), "%d", jord && cJSON_IsNumber(jord) ? jord->valueint : 0);
    snprintf(cp_s, sizeof(cp_s), "%d", jcp && cJSON_IsNumber(jcp) ? jcp->valueint : 0);
    snprintf(pcd_s, sizeof(pcd_s), "%d", jpcd && cJSON_IsNumber(jpcd) ? jpcd->valueint : 15);

    const char *params[] = {
        gid_s,
        jname->valuestring,
        jmeth && cJSON_IsString(jmeth) ? jmeth->valuestring : "ssh_password",
        jhost && cJSON_IsString(jhost) ? jhost->valuestring : "",
        juser && cJSON_IsString(juser) ? juser->valuestring : "",
        jcred && cJSON_IsString(jcred) ? jcred->valuestring : "",
        jcmd->valuestring,
        tmo_s, ord_s,
        jcm && cJSON_IsString(jcm) ? jcm->valuestring : "ping",
        cp_s,
        jcc && cJSON_IsString(jcc) ? jcc->valuestring : "",
        pcd_s,
        id_s,
        NULL
    };
    int rc = db_execute_non_query(ctx->db,
        "UPDATE shutdown_targets SET group_id=?, name=?, method=?, host=?, "
        "username=?, credential=?, command=?, timeout_sec=?, order_in_group=?, "
        "confirm_method=?, confirm_port=?, confirm_command=?, post_confirm_delay=? "
        "WHERE id=?",
        params, NULL);
    cJSON_Delete(body);
    if (rc != 0) return api_error(500, "failed to update target");
    return api_ok_msg("updated");
}

static api_response_t handle_shutdown_targets_delete(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;
    if (!req->body) return api_error(400, "request body required");
    cJSON *body = cJSON_Parse(req->body);
    if (!body) return api_error(400, "invalid JSON");

    const cJSON *jid = cJSON_GetObjectItem(body, "id");
    if (!jid || !cJSON_IsNumber(jid)) { cJSON_Delete(body); return api_error(400, "missing 'id'"); }

    char id_s[16];
    snprintf(id_s, sizeof(id_s), "%d", jid->valueint);

    const char *params[] = { id_s, NULL };
    int rc = db_execute_non_query(ctx->db,
        "DELETE FROM shutdown_targets WHERE id=?", params, NULL);
    cJSON_Delete(body);
    if (rc != 0) return api_error(500, "failed to delete target");
    return api_ok_msg("deleted");
}

static api_response_t handle_shutdown_settings_get(const api_request_t *req, void *ud)
{
    (void)req;
    route_ctx_t *ctx = ud;

    cJSON *obj = cJSON_CreateObject();

    /* Trigger settings */
    cJSON *trig = cJSON_CreateObject();
    const char *tmode = config_get_str(ctx->config, "shutdown.trigger");
    cJSON_AddStringToObject(trig, "mode", tmode ? tmode : "software");
    const char *tsource = config_get_str(ctx->config, "shutdown.trigger_source");
    cJSON_AddStringToObject(trig, "source", tsource ? tsource : "runtime");
    cJSON_AddNumberToObject(trig, "runtime_sec", config_get_int(ctx->config, "shutdown.trigger_runtime_sec", 300));
    cJSON_AddNumberToObject(trig, "battery_pct", config_get_int(ctx->config, "shutdown.trigger_battery_pct", 0));
    cJSON_AddBoolToObject(trig, "on_battery", config_get_int(ctx->config, "shutdown.trigger_on_battery", 1));
    cJSON_AddNumberToObject(trig, "delay_sec", config_get_int(ctx->config, "shutdown.trigger_delay_sec", 30));
    const char *tfield = config_get_str(ctx->config, "shutdown.trigger_field");
    cJSON_AddStringToObject(trig, "field", tfield ? tfield : "");
    const char *tfop = config_get_str(ctx->config, "shutdown.trigger_field_op");
    cJSON_AddStringToObject(trig, "field_op", tfop ? tfop : "lt");
    cJSON_AddNumberToObject(trig, "field_value", config_get_int(ctx->config, "shutdown.trigger_field_value", 0));
    cJSON_AddItemToObject(obj, "trigger", trig);

    /* UPS action */
    cJSON *ups = cJSON_CreateObject();
    const char *mode = config_get_str(ctx->config, "shutdown.ups_mode");
    cJSON_AddStringToObject(ups, "mode", mode ? mode : "command");
    const char *reg = config_get_str(ctx->config, "shutdown.ups_register");
    cJSON_AddStringToObject(ups, "register", reg ? reg : "");
    cJSON_AddNumberToObject(ups, "value", config_get_int(ctx->config, "shutdown.ups_value", 0));
    cJSON_AddNumberToObject(ups, "delay", config_get_int(ctx->config, "shutdown.ups_delay", 5));
    cJSON_AddItemToObject(obj, "ups_action", ups);

    /* Controller */
    cJSON *ctrl = cJSON_CreateObject();
    cJSON_AddBoolToObject(ctrl, "enabled", config_get_int(ctx->config, "shutdown.controller_enabled", 1));
    cJSON_AddItemToObject(obj, "controller", ctrl);

    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return api_ok(json);
}

static api_response_t handle_shutdown_settings_set(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;
    if (!req->body) return api_error(400, "request body required");
    cJSON *body = cJSON_Parse(req->body);
    if (!body) return api_error(400, "invalid JSON");

    const cJSON *trigger = cJSON_GetObjectItem(body, "trigger");
    if (trigger) {
        const cJSON *jm = cJSON_GetObjectItem(trigger, "mode");
        if (jm && cJSON_IsString(jm))
            config_set_db(ctx->config, "shutdown.trigger", jm->valuestring);
        const cJSON *jsrc = cJSON_GetObjectItem(trigger, "source");
        if (jsrc && cJSON_IsString(jsrc))
            config_set_db(ctx->config, "shutdown.trigger_source", jsrc->valuestring);
        const cJSON *jrt = cJSON_GetObjectItem(trigger, "runtime_sec");
        if (jrt && cJSON_IsNumber(jrt)) {
            char v[16]; snprintf(v, sizeof(v), "%d", jrt->valueint);
            config_set_db(ctx->config, "shutdown.trigger_runtime_sec", v);
        }
        const cJSON *jbp = cJSON_GetObjectItem(trigger, "battery_pct");
        if (jbp && cJSON_IsNumber(jbp)) {
            char v[16]; snprintf(v, sizeof(v), "%d", jbp->valueint);
            config_set_db(ctx->config, "shutdown.trigger_battery_pct", v);
        }
        const cJSON *job = cJSON_GetObjectItem(trigger, "on_battery");
        if (job && cJSON_IsBool(job))
            config_set_db(ctx->config, "shutdown.trigger_on_battery",
                          cJSON_IsTrue(job) ? "1" : "0");
        const cJSON *jds = cJSON_GetObjectItem(trigger, "delay_sec");
        if (jds && cJSON_IsNumber(jds)) {
            char v[16]; snprintf(v, sizeof(v), "%d", jds->valueint);
            config_set_db(ctx->config, "shutdown.trigger_delay_sec", v);
        }
        const cJSON *jf = cJSON_GetObjectItem(trigger, "field");
        if (jf && cJSON_IsString(jf))
            config_set_db(ctx->config, "shutdown.trigger_field", jf->valuestring);
        const cJSON *jfo = cJSON_GetObjectItem(trigger, "field_op");
        if (jfo && cJSON_IsString(jfo))
            config_set_db(ctx->config, "shutdown.trigger_field_op", jfo->valuestring);
        const cJSON *jfv = cJSON_GetObjectItem(trigger, "field_value");
        if (jfv && cJSON_IsNumber(jfv)) {
            char v[16]; snprintf(v, sizeof(v), "%d", jfv->valueint);
            config_set_db(ctx->config, "shutdown.trigger_field_value", v);
        }
    }

    const cJSON *ups_action = cJSON_GetObjectItem(body, "ups_action");
    if (ups_action) {
        const cJSON *jmode = cJSON_GetObjectItem(ups_action, "mode");
        const cJSON *jreg  = cJSON_GetObjectItem(ups_action, "register");
        const cJSON *jval  = cJSON_GetObjectItem(ups_action, "value");
        const cJSON *jdly  = cJSON_GetObjectItem(ups_action, "delay");

        if (jmode && cJSON_IsString(jmode))
            config_set_db(ctx->config, "shutdown.ups_mode", jmode->valuestring);
        if (jreg && cJSON_IsString(jreg))
            config_set_db(ctx->config, "shutdown.ups_register", jreg->valuestring);
        if (jval && cJSON_IsNumber(jval)) {
            char v[16]; snprintf(v, sizeof(v), "%d", jval->valueint);
            config_set_db(ctx->config, "shutdown.ups_value", v);
        }
        if (jdly && cJSON_IsNumber(jdly)) {
            char v[16]; snprintf(v, sizeof(v), "%d", jdly->valueint);
            config_set_db(ctx->config, "shutdown.ups_delay", v);
        }
    }

    const cJSON *controller = cJSON_GetObjectItem(body, "controller");
    if (controller) {
        const cJSON *jen = cJSON_GetObjectItem(controller, "enabled");
        if (jen && cJSON_IsBool(jen))
            config_set_db(ctx->config, "shutdown.controller_enabled",
                          cJSON_IsTrue(jen) ? "1" : "0");
    }

    cJSON_Delete(body);
    return api_ok_msg("updated");
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

    /* Try file-backed first, fall back to DB-backed */
    int rc = config_set(ctx->config, jkey->valuestring, jval->valuestring);
    if (rc != 0)
        rc = config_set_db(ctx->config, jkey->valuestring, jval->valuestring);
    cJSON_Delete(body);

    if (rc != 0) return api_error(400, "unknown config key");

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "result", "updated");
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return api_ok(json);
}

/* --- Auth endpoints --- */

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
        params.type = UPS_CONN_SERIAL;
        params.serial.device = jdev->valuestring;
        params.serial.baud = (jbaud && cJSON_IsNumber(jbaud)) ? jbaud->valueint : 9600;
        params.serial.slave_id = (jslave && cJSON_IsNumber(jslave)) ? jslave->valueint : 1;
    }
    cJSON_Delete(body);

    ups_t *test_ups = ups_connect(&params);
    if (!test_ups) {
        return api_error(502, "failed to connect — check device path, baud rate, and that the UPS is powered on");
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

/* --- Command descriptors endpoint --- */

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
    api_server_route(srv, "/api/commands",     API_GET,  handle_commands,       ctx);
    api_server_route(srv, "/api/cmd",          API_POST, handle_cmd,            ctx);
    api_server_route(srv, "/api/events",       API_GET,  handle_events,         ctx);
    api_server_route(srv, "/api/telemetry",    API_GET,  handle_telemetry,      ctx);
    api_server_route(srv, "/api/config/ups*",  API_GET,  handle_config_ups_get, ctx);
    api_server_route(srv, "/api/config/ups",   API_POST, handle_config_ups_set, ctx);
    api_server_route(srv, "/api/shutdown/groups",   API_GET,    handle_shutdown_groups_get,    ctx);
    api_server_route(srv, "/api/shutdown/groups",   API_POST,   handle_shutdown_groups_post,   ctx);
    api_server_route(srv, "/api/shutdown/groups",   API_PUT,    handle_shutdown_groups_put,    ctx);
    api_server_route(srv, "/api/shutdown/groups",   API_DELETE, handle_shutdown_groups_delete, ctx);
    api_server_route(srv, "/api/shutdown/targets",  API_GET,    handle_shutdown_targets_get,   ctx);
    api_server_route(srv, "/api/shutdown/targets",  API_POST,   handle_shutdown_targets_post,  ctx);
    api_server_route(srv, "/api/shutdown/targets",  API_PUT,    handle_shutdown_targets_put,   ctx);
    api_server_route(srv, "/api/shutdown/targets",  API_DELETE, handle_shutdown_targets_delete, ctx);
    api_server_route(srv, "/api/shutdown/settings", API_GET,    handle_shutdown_settings_get,  ctx);
    api_server_route(srv, "/api/shutdown/settings", API_POST,   handle_shutdown_settings_set,  ctx);
    api_server_route(srv, "/api/config/app",   API_GET,  handle_app_config_get, ctx);
    api_server_route(srv, "/api/config/app",   API_POST, handle_app_config_set, ctx);
    api_server_route(srv, "/api/restart",      API_POST, handle_restart,        ctx);
    api_server_route(srv, "/api/auth/setup",   API_POST, handle_auth_setup, ctx);
    api_server_route(srv, "/api/auth/login",   API_POST, handle_auth_login, ctx);
    api_server_route(srv, "/api/auth/change", API_POST, handle_auth_change, ctx);
    api_server_route(srv, "/api/auth/logout", API_POST, handle_auth_logout, ctx);
    api_server_route(srv, "/api/setup/status", API_GET,  handle_setup_status, ctx);
    api_server_route(srv, "/api/setup/ports",  API_GET,  handle_setup_ports,  ctx);
    api_server_route(srv, "/api/setup/test",   API_POST, handle_setup_test,   ctx);
    api_server_route(srv, "/api/weather/status", API_GET,  handle_weather_status,     ctx);
    api_server_route(srv, "/api/weather/config", API_GET,  handle_weather_config_get, ctx);
    api_server_route(srv, "/api/weather/config", API_POST, handle_weather_config_set, ctx);
}
