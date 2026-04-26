#include "api/routes/routes.h"
#include "config/app_config.h"
#include <cutils/db.h>
#include <cutils/error.h>
#include <cutils/json.h>
#include <cutils/mem.h>

/* config.c is a mixed file. Request parsing (the UAF-prone side) goes
 * through cutils/json. Response building stays on cJSON where the
 * output is either a bare top-level array (frontends consume these
 * directly) or a complex nested builder (reg_to_json, the out_of_range
 * error object) — those shapes don't yet have a clean cu_json
 * equivalent, and they're pure build-side code so no UAF class applies.
 *
 * The cu_json fence (c-utils!8) blocks cJSON.h by default; this file
 * legitimately needs it for the two builder sites above. */
#define CUTILS_CJSON_ALLOW
#include <cJSON.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* --- UPS config register JSON --- */

static const char *category_name(ups_reg_category_t c)
{
    switch (c) {
    case UPS_REG_CATEGORY_CONFIG:      return "config";
    case UPS_REG_CATEGORY_MEASUREMENT: return "measurement";
    case UPS_REG_CATEGORY_IDENTITY:    return "identity";
    case UPS_REG_CATEGORY_DIAGNOSTIC:  return "diagnostic";
    }
    return "config";
}

static cJSON *reg_to_json(const ups_config_reg_t *reg, uint32_t raw,
                          const char *str_val)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "name", reg->name);
    cJSON_AddStringToObject(obj, "display_name", reg->display_name);
    if (reg->unit) cJSON_AddStringToObject(obj, "unit", reg->unit);
    if (reg->group) cJSON_AddStringToObject(obj, "group", reg->group);
    cJSON_AddStringToObject(obj, "category", category_name(reg->category));
    cJSON_AddNumberToObject(obj, "raw_value", (double)raw);
    cJSON_AddBoolToObject(obj, "writable", reg->writable);

    /* Sentinel: driver flagged this raw value as "not applicable on this
     * firmware/SKU" (e.g. SMT bypass voltage = 0xFFFF, load shed = 0x0000).
     * Frontend renders is_sentinel as "N/A" instead of decoding. */
    int is_sentinel = reg->has_sentinel && raw == reg->sentinel_value;
    if (is_sentinel)
        cJSON_AddBoolToObject(obj, "is_sentinel", true);

    double scaled = (reg->scale > 1) ? (double)raw / reg->scale : (double)raw;
    cJSON_AddNumberToObject(obj, "value", scaled);

    /* Add human-readable date for date fields */
    if (reg->unit && strcmp(reg->unit, "days since 2000-01-01") == 0 && raw > 0
        && !is_sentinel) {
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
    } else if (reg->type == UPS_CFG_FLAGS) {
        /* Multi-bit field: emit every opts[] entry whose value bits are
         * set in raw. Used for status / error registers where many bits
         * can be active simultaneously (e.g. UPS Status: OnLine | HE Mode). */
        cJSON_AddStringToObject(obj, "type", "flags");
        cJSON *active = cJSON_CreateArray();
        for (size_t i = 0; i < reg->meta.bitfield.count; i++) {
            uint32_t mask = reg->meta.bitfield.opts[i].value;
            if (mask != 0 && (raw & mask) == mask) {
                cJSON *flag = cJSON_CreateObject();
                cJSON_AddNumberToObject(flag, "value", mask);
                cJSON_AddStringToObject(flag, "name",  reg->meta.bitfield.opts[i].name);
                cJSON_AddStringToObject(flag, "label", reg->meta.bitfield.opts[i].label);
                cJSON_AddItemToArray(active, flag);
            }
        }
        cJSON_AddItemToObject(obj, "active_flags", active);
        /* Also emit the full opts[] catalog so the UI can show "this bit
         * is defined but not currently set" if it wants to. */
        cJSON *opts = cJSON_CreateArray();
        for (size_t i = 0; i < reg->meta.bitfield.count; i++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "value", reg->meta.bitfield.opts[i].value);
            cJSON_AddStringToObject(o, "name",  reg->meta.bitfield.opts[i].name);
            cJSON_AddStringToObject(o, "label", reg->meta.bitfield.opts[i].label);
            cJSON_AddItemToArray(opts, o);
        }
        cJSON_AddItemToObject(obj, "options", opts);
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

        uint32_t raw = 0;
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

    /* /api/config/ups is the operator-facing settings list — filter to
     * CONFIG-category entries so MEASUREMENT/IDENTITY/DIAGNOSTIC don't
     * flood the page. /api/about is the "everything" view. */
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        if (regs[i].category != UPS_REG_CATEGORY_CONFIG) continue;

        uint32_t raw = 0;
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
    CUTILS_AUTO_DBRES db_result_t *res = NULL;
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

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return api_ok(json);
}

static api_response_t handle_config_ups_set(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;
    if (!ctx->ups) return api_error(503, "no UPS connected");
    if (!req->body) return api_error(400, "request body required");

    CUTILS_AUTO_JSON_REQ cutils_json_req_t *body = NULL;
    if (json_req_parse(req->body, req->body_len, &body) != CUTILS_OK)
        return api_error(400, "invalid JSON");

    CUTILS_AUTOFREE char *name = NULL;
    int32_t val32;
    if (json_req_get_str(body, "name",  &name)  != CUTILS_OK ||
        json_req_get_i32(body, "value", &val32, 0, 65535) != CUTILS_OK)
        return api_error(400, "missing 'name' (string) and 'value' (number in [0,65535])");

    const ups_config_reg_t *reg = ups_find_config_reg(ctx->ups, name);
    if (!reg)           return api_error(404, "register not found");
    if (!reg->writable) return api_error(400, "register is read-only");

    uint16_t val = (uint16_t)val32;

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

    uint32_t readback = 0;
    /* Best-effort readback for the snapshot insert below; a failure here
     * just means the "display_value" will reflect 0 in the audit trail. */
    CUTILS_UNUSED(ups_config_read(ctx->ups, reg, &readback, NULL, 0));

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
        if (readback != (uint32_t)val)
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

/* --- About (device facts) endpoint ---
 * Co-located with config because it reuses reg_to_json. Returns the full
 * picture of what the driver knows about the device: inventory
 * (ups_inventory_t identity fields) + every config_reg the driver
 * exposes (writable and read-only alike). Read-only view; the UI does
 * not offer edits here — that's what the dedicated config page is for. */

static api_response_t handle_about(const api_request_t *req, void *ud)
{
    (void)req;
    route_ctx_t *ctx = ud;

    cJSON *root = cJSON_CreateObject();

    if (ctx->monitor) {
        ups_inventory_t inv;
        if (monitor_get_inventory(ctx->monitor, &inv) == 0) {
            cJSON *inventory = cJSON_CreateObject();
            cJSON_AddStringToObject(inventory, "model",         inv.model);
            cJSON_AddStringToObject(inventory, "sku",           inv.sku);
            cJSON_AddStringToObject(inventory, "serial",        inv.serial);
            cJSON_AddStringToObject(inventory, "firmware",      inv.firmware);
            cJSON_AddNumberToObject(inventory, "nominal_va",    inv.nominal_va);
            cJSON_AddNumberToObject(inventory, "nominal_watts", inv.nominal_watts);
            cJSON_AddNumberToObject(inventory, "sog_config",    inv.sog_config);
            cJSON_AddItemToObject(root, "inventory", inventory);
        }
    }

    cJSON *registers = cJSON_CreateArray();
    if (ctx->ups) {
        size_t count;
        const ups_config_reg_t *regs = ups_get_config_regs(ctx->ups, &count);
        for (size_t i = 0; i < count; i++) {
            uint32_t raw = 0;
            char str_buf[64] = "";
            if (ups_config_read(ctx->ups, &regs[i], &raw,
                                regs[i].type == UPS_CFG_STRING ? str_buf : NULL,
                                sizeof(str_buf)) == 0)
                cJSON_AddItemToArray(registers, reg_to_json(&regs[i], raw,
                    regs[i].type == UPS_CFG_STRING ? str_buf : NULL));
        }
    }
    cJSON_AddItemToObject(root, "registers", registers);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
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

    CUTILS_AUTO_DBRES db_result_t *result = NULL;
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
    }

    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return api_ok(json);
}

static api_response_t handle_app_config_set(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;
    if (!req->body) return api_error(400, "request body required");

    CUTILS_AUTO_JSON_REQ cutils_json_req_t *body = NULL;
    if (json_req_parse(req->body, req->body_len, &body) != CUTILS_OK)
        return api_error(400, "invalid JSON");

    CUTILS_AUTOFREE char *key   = NULL;
    CUTILS_AUTOFREE char *value = NULL;
    if (json_req_get_str(body, "key",   &key)   != CUTILS_OK ||
        json_req_get_str(body, "value", &value) != CUTILS_OK)
        return api_error(400, "missing 'key' and 'value' strings");

    /* config_set is file-backed and doesn't touch DB; config_set_db does.
     * Wrapping both lets us roll back the DB write if the response-path
     * fails. Also gives the handler a uniform serialization story. */
    CUTILS_AUTO_DB_TX cutils_db_tx_t tx = { 0 };
    if (cutils_db_tx_begin_immediate(ctx->db, &tx) != CUTILS_OK)
        return api_error(500, cutils_get_error());

    int rc = config_set(ctx->config, key, value);
    if (rc != CUTILS_OK)
        rc = config_set_db(ctx->config, key, value);

    if (rc != CUTILS_OK) return api_error(400, "unknown config key");

    if (db_tx_commit(&tx) != CUTILS_OK)
        return api_error(500, cutils_get_error());
    return api_ok_msg("updated");
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

    api_server_route(srv, "/api/about",        API_GET,  handle_about,          ctx);
}
