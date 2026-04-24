#include "api/routes/routes.h"
#include "config/app_config.h"
#include <cJSON.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* --- UPS config register JSON --- */

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
        time_t epoch = 946684800 + (time_t)raw * 86400;
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
        if (reg->meta.scalar.step != 0)
            cJSON_AddNumberToObject(obj, "step", reg->meta.scalar.step);
    } else if (reg->type == UPS_CFG_BITFIELD) {
        cJSON_AddStringToObject(obj, "type", "bitfield");
        for (size_t i = 0; i < reg->meta.bitfield.count; i++) {
            if (reg->meta.bitfield.opts[i].value == raw) {
                cJSON_AddStringToObject(obj, "setting", reg->meta.bitfield.opts[i].name);
                cJSON_AddStringToObject(obj, "setting_label", reg->meta.bitfield.opts[i].label);
                break;
            }
        }
        cJSON *opts = cJSON_CreateArray();
        for (size_t i = 0; i < reg->meta.bitfield.count; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "value", reg->meta.bitfield.opts[i].value);
            cJSON_AddStringToObject(o, "name", reg->meta.bitfield.opts[i].name);
            cJSON_AddStringToObject(o, "label", reg->meta.bitfield.opts[i].label);
            cJSON_AddItemToArray(opts, o);
        }
        cJSON_AddItemToObject(obj, "options", opts);
        cJSON_AddBoolToObject(obj, "strict", reg->meta.bitfield.strict);
    } else if (reg->type == UPS_CFG_STRING) {
        cJSON_AddStringToObject(obj, "type", "string");
        if (reg->meta.string.max_chars != 0)
            cJSON_AddNumberToObject(obj, "max_chars", (double)reg->meta.string.max_chars);
        if (str_val)
            cJSON_AddStringToObject(obj, "string_value", str_val);
    }

    return obj;
}

/* --- UPS config endpoints --- */

static api_response_t handle_config_ups_get(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;
    if (!ctx->ups) return api_ok(strdup("[]"));

    const char *name = NULL;
    if (strlen(req->url) > 16)
        name = req->url + 16;

    if (name && name[0]) {
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

static api_response_t handle_config_ups_history(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;
    const char *name = api_query_param(req, "name");
    if (!name || !name[0])
        return api_error(400, "missing 'name' query parameter");

    /* Validate against the driver's register set — keeps the endpoint honest
     * (no arbitrary SQL injection via register_name) and gives a clean 404
     * when a stale frontend asks about a register that no longer exists. */
    if (ctx->ups && !ups_find_config_reg(ctx->ups, name))
        return api_error(404, "register not found");

    const char *params[] = { name, NULL };
    db_result_t *res = NULL;
    int rc = db_execute(ctx->db,
        "SELECT timestamp, raw_value, display_value, source FROM ups_config "
        "WHERE register_name = ? ORDER BY id DESC",
        params, &res);

    cJSON *arr = cJSON_CreateArray();
    if (rc == 0 && res) {
        for (int i = 0; i < res->nrows; i++) {
            cJSON *row = cJSON_CreateObject();
            cJSON_AddStringToObject(row, "timestamp",     res->rows[i][0]);
            cJSON_AddNumberToObject(row, "raw_value",     (double)strtol(res->rows[i][1], NULL, 10));
            cJSON_AddStringToObject(row, "display_value", res->rows[i][2] ? res->rows[i][2] : "");
            if (res->rows[i][3])
                cJSON_AddStringToObject(row, "source",    res->rows[i][3]);
            else
                cJSON_AddNullToObject(row, "source");
            cJSON_AddItemToArray(arr, row);
        }
    }
    db_result_free(res);

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

    /* Validation failure is an operator error, not a UPS/network error —
     * surface it as HTTP 400 with enough metadata for the frontend to
     * render an inline explanation without re-fetching the register. */
    if (rc == UPS_ERR_INVALID_VALUE) {
        cJSON *err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "error", "out_of_range");
        cJSON_AddStringToObject(err, "register", reg->name);
        cJSON_AddNumberToObject(err, "attempted_value", val);
        if (reg->type == UPS_CFG_SCALAR) {
            if (reg->meta.scalar.min != 0 || reg->meta.scalar.max != 0) {
                cJSON_AddNumberToObject(err, "min", reg->meta.scalar.min);
                cJSON_AddNumberToObject(err, "max", reg->meta.scalar.max);
            }
            if (reg->meta.scalar.step != 0)
                cJSON_AddNumberToObject(err, "step", reg->meta.scalar.step);
        } else if (reg->type == UPS_CFG_BITFIELD) {
            cJSON *accepted = cJSON_CreateArray();
            for (size_t i = 0; i < reg->meta.bitfield.count; i++)
                cJSON_AddItemToArray(accepted,
                    cJSON_CreateNumber(reg->meta.bitfield.opts[i].value));
            cJSON_AddItemToObject(err, "accepted_values", accepted);
        }
        char *ejson = cJSON_PrintUnformatted(err);
        cJSON_Delete(err);
        return api_ok_status(400, ejson);
    }

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
        const char *snap_params[] = { reg->name, raw_s, display_s, ts, "api", NULL };
        /* Best-effort audit snapshot; the register write above is the
         * primary operation and has already succeeded by this point. */
        CUTILS_UNUSED(db_execute_non_query(ctx->db,
            "INSERT INTO ups_config "
            "(register_name, raw_value, display_value, timestamp, source) "
            "VALUES (?, ?, ?, ?, ?)",
            snap_params, NULL));
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

    if (strcmp(reg->name, "transfer_high") == 0 ||
        strcmp(reg->name, "transfer_low") == 0)
        api_refresh_thresholds(ctx);

    return api_ok(json);
}

/* --- App config endpoints --- */

static api_response_t handle_app_config_get(const api_request_t *req, void *ud)
{
    (void)req;
    route_ctx_t *ctx = ud;

    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; app_file_keys[i].key; i++) {
        const config_key_t *k = &app_file_keys[i];
        const char *val = config_get_str(ctx->config, k->key);
        cJSON *c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "key", k->key);
        cJSON_AddStringToObject(c, "value", val ? val : k->default_value);
        cJSON_AddStringToObject(c, "type", k->type == CFG_INT ? "int" : "string");
        cJSON_AddStringToObject(c, "default_value", k->default_value);
        cJSON_AddStringToObject(c, "description", k->description);
        cJSON_AddItemToArray(arr, c);
    }

    db_result_t *result = NULL;
    int rc = db_execute(ctx->db,
        "SELECT key, value, type, default_value, description FROM config ORDER BY key",
        NULL, &result);
    if (rc == 0 && result) {
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
    }

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

/* --- Registration --- */

void api_register_config_routes(api_server_t *srv, route_ctx_t *ctx)
{
    /* Registered before the /api/config/ups* wildcard — the router matches
     * in registration order, so this exact path wins over the prefix. */
    api_server_route(srv, "/api/config/ups/history", API_GET, handle_config_ups_history, ctx);
    api_server_route(srv, "/api/config/ups*",  API_GET,  handle_config_ups_get, ctx);
    api_server_route(srv, "/api/config/ups",   API_POST, handle_config_ups_set, ctx);
    api_server_route(srv, "/api/config/app",   API_GET,  handle_app_config_get, ctx);
    api_server_route(srv, "/api/config/app",   API_POST, handle_app_config_set, ctx);
}
