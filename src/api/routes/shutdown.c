#include "api/routes/routes.h"
#include <cJSON.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* --- Shutdown group CRUD --- */

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

    const char *params[] = { id_s, NULL };
    int rc = db_execute_non_query(ctx->db,
        "DELETE FROM shutdown_groups WHERE id=?", params, NULL);
    cJSON_Delete(body);
    if (rc != 0) return api_error(500, "failed to delete group");
    return api_ok_msg("deleted");
}

/* --- Shutdown target CRUD --- */

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

/* --- Shutdown settings --- */

static api_response_t handle_shutdown_settings_get(const api_request_t *req, void *ud)
{
    (void)req;
    route_ctx_t *ctx = ud;

    cJSON *obj = cJSON_CreateObject();

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

    cJSON *ups = cJSON_CreateObject();
    const char *mode = config_get_str(ctx->config, "shutdown.ups_mode");
    cJSON_AddStringToObject(ups, "mode", mode ? mode : "command");
    const char *reg = config_get_str(ctx->config, "shutdown.ups_register");
    cJSON_AddStringToObject(ups, "register", reg ? reg : "");
    cJSON_AddNumberToObject(ups, "value", config_get_int(ctx->config, "shutdown.ups_value", 0));
    cJSON_AddNumberToObject(ups, "delay", config_get_int(ctx->config, "shutdown.ups_delay", 5));
    cJSON_AddItemToObject(obj, "ups_action", ups);

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
