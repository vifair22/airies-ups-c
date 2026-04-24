#include "api/routes/routes.h"
#include <cutils/error.h>
#include <cutils/json.h>
#include <cutils/mem.h>

/* Top-level bare-array responses (handle_shutdown_groups_get and
 * handle_shutdown_targets_get) stay on cJSON — cu_json roots are
 * objects only. Those two handlers are build-side code, no UAF
 * class applies. */
#include <cJSON.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Propagate a JSON builder failure as 500 + cutils_get_error(). */
#define ADD_OR_FAIL(expr) \
    do { \
        if ((expr) != CUTILS_OK) return api_error(500, cutils_get_error()); \
    } while (0)

/* Write a config value, returning 500 on failure. */
#define CFG_SET_OR_FAIL(key, val) \
    do { \
        if (config_set_db(ctx->config, (key), (val)) != CUTILS_OK) \
            return api_error(500, cutils_get_error()); \
    } while (0)

static api_response_t finalize_ok(cutils_json_resp_t *resp)
{
    CUTILS_AUTOFREE char *json = NULL;
    size_t len;
    if (json_resp_finalize(resp, &json, &len) != CUTILS_OK)
        return api_error(500, cutils_get_error());
    return api_ok(CUTILS_MOVE(json));
}

/* --- Shutdown group CRUD --- */

static api_response_t handle_shutdown_groups_get(const api_request_t *req, void *ud)
{
    (void)req;
    route_ctx_t *ctx = ud;
    CUTILS_AUTO_DBRES db_result_t *result = NULL;
    int rc = db_execute(ctx->db,
        "SELECT id, name, execution_order, parallel, max_timeout_sec, post_group_delay "
        "FROM shutdown_groups ORDER BY execution_order",
        NULL, &result);
    if (rc != CUTILS_OK || !result) return api_error(500, "failed to query groups");

    /* Top-level array — see file-head note. */
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < result->nrows; i++) {
        cJSON *g = cJSON_CreateObject();
        cJSON_AddNumberToObject(g, "id",               atoi(result->rows[i][0]));
        cJSON_AddStringToObject(g, "name",             result->rows[i][1]);
        cJSON_AddNumberToObject(g, "execution_order",  atoi(result->rows[i][2]));
        cJSON_AddBoolToObject  (g, "parallel",         atoi(result->rows[i][3]));
        cJSON_AddNumberToObject(g, "max_timeout_sec",  atoi(result->rows[i][4]));
        cJSON_AddNumberToObject(g, "post_group_delay", atoi(result->rows[i][5]));
        cJSON_AddItemToArray(arr, g);
    }
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return api_ok(json);
}

static api_response_t handle_shutdown_groups_post(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;
    if (!req->body) return api_error(400, "request body required");

    CUTILS_AUTO_JSON_REQ cutils_json_req_t *body = NULL;
    if (json_req_parse(req->body, req->body_len, &body) != CUTILS_OK)
        return api_error(400, "invalid JSON");

    CUTILS_AUTOFREE char *name = NULL;
    if (json_req_get_str(body, "name", &name) != CUTILS_OK)
        return api_error(400, "missing 'name'");

    int32_t order = 0, mtmo = 0, pgd = 0;
    bool par = true;
    CUTILS_UNUSED(json_req_get_i32 (body, "execution_order",  &order, INT32_MIN, INT32_MAX));
    CUTILS_UNUSED(json_req_get_bool(body, "parallel",         &par));
    CUTILS_UNUSED(json_req_get_i32 (body, "max_timeout_sec",  &mtmo,  INT32_MIN, INT32_MAX));
    CUTILS_UNUSED(json_req_get_i32 (body, "post_group_delay", &pgd,   INT32_MIN, INT32_MAX));

    char order_s[16], par_s[4], mtmo_s[16], pgd_s[16];
    snprintf(order_s, sizeof(order_s), "%d", order);
    snprintf(par_s,   sizeof(par_s),   "%d", par ? 1 : 0);
    snprintf(mtmo_s,  sizeof(mtmo_s),  "%d", mtmo);
    snprintf(pgd_s,   sizeof(pgd_s),   "%d", pgd);

    const char *params[] = { name, order_s, par_s, mtmo_s, pgd_s, NULL };
    int rc = db_execute_non_query(ctx->db,
        "INSERT INTO shutdown_groups (name, execution_order, parallel, "
        "max_timeout_sec, post_group_delay) VALUES (?, ?, ?, ?, ?)",
        params, NULL);
    if (rc != CUTILS_OK) return api_error(500, "failed to create group");

    CUTILS_AUTO_JSON_RESP cutils_json_resp_t *resp = NULL;
    if (json_resp_new(&resp) != CUTILS_OK)
        return api_error(500, cutils_get_error());
    ADD_OR_FAIL(json_resp_add_str(resp, "result", "created"));

    CUTILS_AUTOFREE char *json = NULL;
    size_t len;
    if (json_resp_finalize(resp, &json, &len) != CUTILS_OK)
        return api_error(500, cutils_get_error());
    return api_ok_status(201, CUTILS_MOVE(json));
}

static api_response_t handle_shutdown_groups_put(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;
    if (!req->body) return api_error(400, "request body required");

    CUTILS_AUTO_JSON_REQ cutils_json_req_t *body = NULL;
    if (json_req_parse(req->body, req->body_len, &body) != CUTILS_OK)
        return api_error(400, "invalid JSON");

    int32_t id;
    if (json_req_get_i32(body, "id", &id, INT32_MIN, INT32_MAX) != CUTILS_OK)
        return api_error(400, "missing 'id'");

    CUTILS_AUTOFREE char *name = NULL;
    if (json_req_get_str(body, "name", &name) != CUTILS_OK)
        return api_error(400, "missing 'name'");

    int32_t order = 0, mtmo = 0, pgd = 0;
    bool par = true;
    CUTILS_UNUSED(json_req_get_i32 (body, "execution_order",  &order, INT32_MIN, INT32_MAX));
    CUTILS_UNUSED(json_req_get_bool(body, "parallel",         &par));
    CUTILS_UNUSED(json_req_get_i32 (body, "max_timeout_sec",  &mtmo,  INT32_MIN, INT32_MAX));
    CUTILS_UNUSED(json_req_get_i32 (body, "post_group_delay", &pgd,   INT32_MIN, INT32_MAX));

    char id_s[16], order_s[16], par_s[4], mtmo_s[16], pgd_s[16];
    snprintf(id_s,    sizeof(id_s),    "%d", id);
    snprintf(order_s, sizeof(order_s), "%d", order);
    snprintf(par_s,   sizeof(par_s),   "%d", par ? 1 : 0);
    snprintf(mtmo_s,  sizeof(mtmo_s),  "%d", mtmo);
    snprintf(pgd_s,   sizeof(pgd_s),   "%d", pgd);

    const char *params[] = { name, order_s, par_s, mtmo_s, pgd_s, id_s, NULL };
    int rc = db_execute_non_query(ctx->db,
        "UPDATE shutdown_groups SET name=?, execution_order=?, parallel=?, "
        "max_timeout_sec=?, post_group_delay=? WHERE id=?",
        params, NULL);
    if (rc != CUTILS_OK) return api_error(500, "failed to update group");
    return api_ok_msg("updated");
}

static api_response_t handle_shutdown_groups_delete(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;
    if (!req->body) return api_error(400, "request body required");

    CUTILS_AUTO_JSON_REQ cutils_json_req_t *body = NULL;
    if (json_req_parse(req->body, req->body_len, &body) != CUTILS_OK)
        return api_error(400, "invalid JSON");

    int32_t id;
    if (json_req_get_i32(body, "id", &id, INT32_MIN, INT32_MAX) != CUTILS_OK)
        return api_error(400, "missing 'id'");

    char id_s[16];
    snprintf(id_s, sizeof(id_s), "%d", id);

    const char *params[] = { id_s, NULL };
    int rc = db_execute_non_query(ctx->db,
        "DELETE FROM shutdown_groups WHERE id=?", params, NULL);
    if (rc != CUTILS_OK) return api_error(500, "failed to delete group");
    return api_ok_msg("deleted");
}

/* --- Shutdown target CRUD --- */

static api_response_t handle_shutdown_targets_get(const api_request_t *req, void *ud)
{
    (void)req;
    route_ctx_t *ctx = ud;
    CUTILS_AUTO_DBRES db_result_t *result = NULL;
    int rc = db_execute(ctx->db,
        "SELECT t.id, t.name, t.method, t.host, t.username, t.command, "
        "t.timeout_sec, t.order_in_group, g.name AS group_name, "
        "t.confirm_method, t.confirm_port, t.confirm_command, t.post_confirm_delay, "
        "t.group_id "
        "FROM shutdown_targets t JOIN shutdown_groups g ON t.group_id = g.id "
        "ORDER BY g.execution_order, t.order_in_group",
        NULL, &result);
    if (rc != CUTILS_OK || !result) return api_error(500, "failed to query targets");

    /* Top-level array — see file-head note. */
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < result->nrows; i++) {
        cJSON *t = cJSON_CreateObject();
        cJSON_AddNumberToObject(t, "id",                 atoi(result->rows[i][0]));
        cJSON_AddStringToObject(t, "name",               result->rows[i][1]);
        cJSON_AddStringToObject(t, "method",             result->rows[i][2]);
        cJSON_AddStringToObject(t, "host",               result->rows[i][3] ? result->rows[i][3] : "");
        cJSON_AddStringToObject(t, "username",           result->rows[i][4] ? result->rows[i][4] : "");
        cJSON_AddStringToObject(t, "command",            result->rows[i][5]);
        cJSON_AddNumberToObject(t, "timeout_sec",        atoi(result->rows[i][6]));
        cJSON_AddNumberToObject(t, "order_in_group",     atoi(result->rows[i][7]));
        cJSON_AddStringToObject(t, "group",              result->rows[i][8]);
        cJSON_AddStringToObject(t, "confirm_method",     result->rows[i][9] ? result->rows[i][9] : "ping");
        cJSON_AddNumberToObject(t, "confirm_port",       result->rows[i][10] ? atoi(result->rows[i][10]) : 0);
        cJSON_AddStringToObject(t, "confirm_command",    result->rows[i][11] ? result->rows[i][11] : "");
        cJSON_AddNumberToObject(t, "post_confirm_delay", atoi(result->rows[i][12]));
        cJSON_AddNumberToObject(t, "group_id",           atoi(result->rows[i][13]));
        cJSON_AddItemToArray(arr, t);
    }
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return api_ok(json);
}

static api_response_t handle_shutdown_targets_post(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;
    if (!req->body) return api_error(400, "request body required");

    CUTILS_AUTO_JSON_REQ cutils_json_req_t *body = NULL;
    if (json_req_parse(req->body, req->body_len, &body) != CUTILS_OK)
        return api_error(400, "invalid JSON");

    int32_t gid;
    CUTILS_AUTOFREE char *name = NULL;
    CUTILS_AUTOFREE char *cmd  = NULL;
    if (json_req_get_i32(body, "group_id", &gid, INT32_MIN, INT32_MAX) != CUTILS_OK ||
        json_req_get_str(body, "name",    &name) != CUTILS_OK ||
        json_req_get_str(body, "command", &cmd)  != CUTILS_OK)
        return api_error(400, "missing required fields: group_id, name, command");

    CUTILS_AUTOFREE char *method = NULL;
    CUTILS_AUTOFREE char *host   = NULL;
    CUTILS_AUTOFREE char *user   = NULL;
    CUTILS_AUTOFREE char *cred   = NULL;
    CUTILS_AUTOFREE char *confm  = NULL;
    CUTILS_AUTOFREE char *confc  = NULL;
    int32_t tmo = 180, cp = 0, pcd = 15;
    CUTILS_UNUSED(json_req_get_str_opt(body, "method",             &method));
    CUTILS_UNUSED(json_req_get_str_opt(body, "host",               &host));
    CUTILS_UNUSED(json_req_get_str_opt(body, "username",           &user));
    CUTILS_UNUSED(json_req_get_str_opt(body, "credential",         &cred));
    CUTILS_UNUSED(json_req_get_str_opt(body, "confirm_method",     &confm));
    CUTILS_UNUSED(json_req_get_str_opt(body, "confirm_command",    &confc));
    CUTILS_UNUSED(json_req_get_i32    (body, "timeout_sec",        &tmo, INT32_MIN, INT32_MAX));
    CUTILS_UNUSED(json_req_get_i32    (body, "confirm_port",       &cp,  INT32_MIN, INT32_MAX));
    CUTILS_UNUSED(json_req_get_i32    (body, "post_confirm_delay", &pcd, INT32_MIN, INT32_MAX));

    char gid_s[16], tmo_s[16], cp_s[16], pcd_s[16];
    snprintf(gid_s, sizeof(gid_s), "%d", gid);
    snprintf(tmo_s, sizeof(tmo_s), "%d", tmo);
    snprintf(cp_s,  sizeof(cp_s),  "%d", cp);
    snprintf(pcd_s, sizeof(pcd_s), "%d", pcd);

    const char *params[] = {
        gid_s,
        name,
        method ? method : "ssh_password",
        host   ? host   : "",
        user   ? user   : "",
        cred   ? cred   : "",
        cmd,
        tmo_s,
        "0",   /* order_in_group — not accepted via POST */
        confm ? confm : "ping",
        cp ? cp_s : "",
        confc ? confc : "",
        pcd_s,
        NULL
    };
    int rc = db_execute_non_query(ctx->db,
        "INSERT INTO shutdown_targets (group_id, name, method, host, username, "
        "credential, command, timeout_sec, order_in_group, "
        "confirm_method, confirm_port, confirm_command, post_confirm_delay) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        params, NULL);
    if (rc != CUTILS_OK) return api_error(500, "failed to create target");

    CUTILS_AUTO_JSON_RESP cutils_json_resp_t *resp = NULL;
    if (json_resp_new(&resp) != CUTILS_OK)
        return api_error(500, cutils_get_error());
    ADD_OR_FAIL(json_resp_add_str(resp, "result", "created"));

    CUTILS_AUTOFREE char *json = NULL;
    size_t len;
    if (json_resp_finalize(resp, &json, &len) != CUTILS_OK)
        return api_error(500, cutils_get_error());
    return api_ok_status(201, CUTILS_MOVE(json));
}

static api_response_t handle_shutdown_targets_put(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;
    if (!req->body) return api_error(400, "request body required");

    CUTILS_AUTO_JSON_REQ cutils_json_req_t *body = NULL;
    if (json_req_parse(req->body, req->body_len, &body) != CUTILS_OK)
        return api_error(400, "invalid JSON");

    int32_t id;
    CUTILS_AUTOFREE char *name = NULL;
    CUTILS_AUTOFREE char *cmd  = NULL;
    if (json_req_get_i32(body, "id",      &id, INT32_MIN, INT32_MAX) != CUTILS_OK ||
        json_req_get_str(body, "name",    &name) != CUTILS_OK ||
        json_req_get_str(body, "command", &cmd)  != CUTILS_OK)
        return api_error(400, "missing required fields: id, name, command");

    CUTILS_AUTOFREE char *method = NULL;
    CUTILS_AUTOFREE char *host   = NULL;
    CUTILS_AUTOFREE char *user   = NULL;
    CUTILS_AUTOFREE char *cred   = NULL;
    CUTILS_AUTOFREE char *confm  = NULL;
    CUTILS_AUTOFREE char *confc  = NULL;
    int32_t gid = 0, tmo = 180, ord = 0, cp = 0, pcd = 15;
    CUTILS_UNUSED(json_req_get_i32    (body, "group_id",           &gid, INT32_MIN, INT32_MAX));
    CUTILS_UNUSED(json_req_get_str_opt(body, "method",             &method));
    CUTILS_UNUSED(json_req_get_str_opt(body, "host",               &host));
    CUTILS_UNUSED(json_req_get_str_opt(body, "username",           &user));
    CUTILS_UNUSED(json_req_get_str_opt(body, "credential",         &cred));
    CUTILS_UNUSED(json_req_get_str_opt(body, "confirm_method",     &confm));
    CUTILS_UNUSED(json_req_get_str_opt(body, "confirm_command",    &confc));
    CUTILS_UNUSED(json_req_get_i32    (body, "timeout_sec",        &tmo, INT32_MIN, INT32_MAX));
    CUTILS_UNUSED(json_req_get_i32    (body, "order_in_group",     &ord, INT32_MIN, INT32_MAX));
    CUTILS_UNUSED(json_req_get_i32    (body, "confirm_port",       &cp,  INT32_MIN, INT32_MAX));
    CUTILS_UNUSED(json_req_get_i32    (body, "post_confirm_delay", &pcd, INT32_MIN, INT32_MAX));

    char id_s[16], gid_s[16], tmo_s[16], ord_s[16], cp_s[16], pcd_s[16];
    snprintf(id_s,  sizeof(id_s),  "%d", id);
    snprintf(gid_s, sizeof(gid_s), "%d", gid);
    snprintf(tmo_s, sizeof(tmo_s), "%d", tmo);
    snprintf(ord_s, sizeof(ord_s), "%d", ord);
    snprintf(cp_s,  sizeof(cp_s),  "%d", cp);
    snprintf(pcd_s, sizeof(pcd_s), "%d", pcd);

    const char *params[] = {
        gid_s,
        name,
        method ? method : "ssh_password",
        host   ? host   : "",
        user   ? user   : "",
        cred   ? cred   : "",
        cmd,
        tmo_s, ord_s,
        confm ? confm : "ping",
        cp_s,
        confc ? confc : "",
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
    if (rc != CUTILS_OK) return api_error(500, "failed to update target");
    return api_ok_msg("updated");
}

static api_response_t handle_shutdown_targets_delete(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;
    if (!req->body) return api_error(400, "request body required");

    CUTILS_AUTO_JSON_REQ cutils_json_req_t *body = NULL;
    if (json_req_parse(req->body, req->body_len, &body) != CUTILS_OK)
        return api_error(400, "invalid JSON");

    int32_t id;
    if (json_req_get_i32(body, "id", &id, INT32_MIN, INT32_MAX) != CUTILS_OK)
        return api_error(400, "missing 'id'");

    char id_s[16];
    snprintf(id_s, sizeof(id_s), "%d", id);

    const char *params[] = { id_s, NULL };
    int rc = db_execute_non_query(ctx->db,
        "DELETE FROM shutdown_targets WHERE id=?", params, NULL);
    if (rc != CUTILS_OK) return api_error(500, "failed to delete target");
    return api_ok_msg("deleted");
}

/* --- Shutdown settings --- */

static api_response_t handle_shutdown_settings_get(const api_request_t *req, void *ud)
{
    (void)req;
    route_ctx_t *ctx = ud;

    CUTILS_AUTO_JSON_RESP cutils_json_resp_t *resp = NULL;
    if (json_resp_new(&resp) != CUTILS_OK)
        return api_error(500, cutils_get_error());

    const char *tmode   = config_get_str(ctx->config, "shutdown.trigger");
    const char *tsource = config_get_str(ctx->config, "shutdown.trigger_source");
    const char *tfield  = config_get_str(ctx->config, "shutdown.trigger_field");
    const char *tfop    = config_get_str(ctx->config, "shutdown.trigger_field_op");

    ADD_OR_FAIL(json_resp_add_str (resp, "trigger.mode",        tmode   ? tmode   : "software"));
    ADD_OR_FAIL(json_resp_add_str (resp, "trigger.source",      tsource ? tsource : "runtime"));
    ADD_OR_FAIL(json_resp_add_i32 (resp, "trigger.runtime_sec",
        config_get_int(ctx->config, "shutdown.trigger_runtime_sec", 300)));
    ADD_OR_FAIL(json_resp_add_i32 (resp, "trigger.battery_pct",
        config_get_int(ctx->config, "shutdown.trigger_battery_pct", 0)));
    ADD_OR_FAIL(json_resp_add_bool(resp, "trigger.on_battery",
        config_get_int(ctx->config, "shutdown.trigger_on_battery", 1) != 0));
    ADD_OR_FAIL(json_resp_add_i32 (resp, "trigger.delay_sec",
        config_get_int(ctx->config, "shutdown.trigger_delay_sec", 30)));
    ADD_OR_FAIL(json_resp_add_str (resp, "trigger.field",       tfield  ? tfield  : ""));
    ADD_OR_FAIL(json_resp_add_str (resp, "trigger.field_op",    tfop    ? tfop    : "lt"));
    ADD_OR_FAIL(json_resp_add_i32 (resp, "trigger.field_value",
        config_get_int(ctx->config, "shutdown.trigger_field_value", 0)));

    const char *mode = config_get_str(ctx->config, "shutdown.ups_mode");
    const char *reg  = config_get_str(ctx->config, "shutdown.ups_register");
    ADD_OR_FAIL(json_resp_add_str(resp, "ups_action.mode",     mode ? mode : "command"));
    ADD_OR_FAIL(json_resp_add_str(resp, "ups_action.register", reg  ? reg  : ""));
    ADD_OR_FAIL(json_resp_add_i32(resp, "ups_action.value",
        config_get_int(ctx->config, "shutdown.ups_value", 0)));
    ADD_OR_FAIL(json_resp_add_i32(resp, "ups_action.delay",
        config_get_int(ctx->config, "shutdown.ups_delay", 5)));

    ADD_OR_FAIL(json_resp_add_bool(resp, "controller.enabled",
        config_get_int(ctx->config, "shutdown.controller_enabled", 1) != 0));

    return finalize_ok(resp);
}

/* Partial-update field helpers, scoped to handle_shutdown_settings_set.
 * Each macro: if the JSON field is present (and right type for opt),
 * write it to the named config key, propagating any write failure as 500. */
#define SET_STR_IF(json_path, cfg_key) \
    do { \
        CUTILS_AUTOFREE char *_v = NULL; \
        CUTILS_UNUSED(json_req_get_str_opt(body, json_path, &_v)); \
        if (_v) CFG_SET_OR_FAIL(cfg_key, _v); \
    } while (0)

#define SET_I32_IF(json_path, cfg_key) \
    do { \
        if (json_req_has(body, json_path)) { \
            int32_t _v; \
            if (json_req_get_i32(body, json_path, &_v, INT32_MIN, INT32_MAX) == CUTILS_OK) { \
                char _s[16]; snprintf(_s, sizeof(_s), "%d", _v); \
                CFG_SET_OR_FAIL(cfg_key, _s); \
            } \
        } \
    } while (0)

#define SET_BOOL_IF(json_path, cfg_key) \
    do { \
        if (json_req_has(body, json_path)) { \
            bool _v; \
            if (json_req_get_bool(body, json_path, &_v) == CUTILS_OK) \
                CFG_SET_OR_FAIL(cfg_key, _v ? "1" : "0"); \
        } \
    } while (0)

static api_response_t handle_shutdown_settings_set(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;
    if (!req->body) return api_error(400, "request body required");

    CUTILS_AUTO_JSON_REQ cutils_json_req_t *body = NULL;
    if (json_req_parse(req->body, req->body_len, &body) != CUTILS_OK)
        return api_error(400, "invalid JSON");

    SET_STR_IF ("trigger.mode",        "shutdown.trigger");
    SET_STR_IF ("trigger.source",      "shutdown.trigger_source");
    SET_I32_IF ("trigger.runtime_sec", "shutdown.trigger_runtime_sec");
    SET_I32_IF ("trigger.battery_pct", "shutdown.trigger_battery_pct");
    SET_BOOL_IF("trigger.on_battery",  "shutdown.trigger_on_battery");
    SET_I32_IF ("trigger.delay_sec",   "shutdown.trigger_delay_sec");
    SET_STR_IF ("trigger.field",       "shutdown.trigger_field");
    SET_STR_IF ("trigger.field_op",    "shutdown.trigger_field_op");
    SET_I32_IF ("trigger.field_value", "shutdown.trigger_field_value");

    SET_STR_IF ("ups_action.mode",     "shutdown.ups_mode");
    SET_STR_IF ("ups_action.register", "shutdown.ups_register");
    SET_I32_IF ("ups_action.value",    "shutdown.ups_value");
    SET_I32_IF ("ups_action.delay",    "shutdown.ups_delay");

    SET_BOOL_IF("controller.enabled",  "shutdown.controller_enabled");

    return api_ok_msg("updated");
}

#undef SET_STR_IF
#undef SET_I32_IF
#undef SET_BOOL_IF
#undef ADD_OR_FAIL
#undef CFG_SET_OR_FAIL

/* --- Registration --- */

void api_register_shutdown_routes(api_server_t *srv, route_ctx_t *ctx)
{
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
}
