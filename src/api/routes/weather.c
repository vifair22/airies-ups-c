#include "api/routes/routes.h"
#include <cutils/db.h>
#include <cutils/error.h>
#include <cutils/json.h>
#include <cutils/mem.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Propagate a JSON builder failure as a 500 with the thread-local
 * error message. Scoped to this file; undef'd at the end. */
#define ADD_OR_FAIL(expr) \
    do { \
        if ((expr) != CUTILS_OK) return api_error(500, cutils_get_error()); \
    } while (0)

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

    CUTILS_AUTO_DBRES db_result_t *result = NULL;
    int rc = db_execute(ctx->db,
        "SELECT enabled, latitude, longitude, alert_zones, alert_types, "
        "wind_speed_mph, severe_keywords, poll_interval, "
        "control_register, severe_raw_value, normal_raw_value "
        "FROM weather_config WHERE id = 1",
        NULL, &result);

    if (rc != CUTILS_OK || !result || result->nrows == 0)
        return api_error(404, "weather not configured");

    char **row = result->rows[0];

    CUTILS_AUTO_JSON_RESP cutils_json_resp_t *resp = NULL;
    if (json_resp_new(&resp) != CUTILS_OK)
        return api_error(500, cutils_get_error());

    ADD_OR_FAIL(json_resp_add_bool(resp, "enabled",          atoi(row[0]) != 0));
    ADD_OR_FAIL(json_resp_add_f64 (resp, "latitude",         atof(row[1])));
    ADD_OR_FAIL(json_resp_add_f64 (resp, "longitude",        atof(row[2])));
    ADD_OR_FAIL(json_resp_add_str (resp, "alert_zones",      row[3] ? row[3] : ""));
    ADD_OR_FAIL(json_resp_add_str (resp, "alert_types",      row[4] ? row[4] : ""));
    ADD_OR_FAIL(json_resp_add_i32 (resp, "wind_speed_mph",   atoi(row[5])));
    ADD_OR_FAIL(json_resp_add_str (resp, "severe_keywords",  row[6] ? row[6] : ""));
    ADD_OR_FAIL(json_resp_add_i32 (resp, "poll_interval",    atoi(row[7])));
    ADD_OR_FAIL(json_resp_add_str (resp, "control_register", row[8] ? row[8] : "freq_tolerance"));
    if (row[9])
        ADD_OR_FAIL(json_resp_add_i32(resp, "severe_raw_value", atoi(row[9])));
    if (row[10])
        ADD_OR_FAIL(json_resp_add_i32(resp, "normal_raw_value", atoi(row[10])));

    CUTILS_AUTOFREE char *json = NULL;
    size_t len;
    if (json_resp_finalize(resp, &json, &len) != CUTILS_OK)
        return api_error(500, cutils_get_error());
    return api_ok(CUTILS_MOVE(json));
}

static api_response_t handle_weather_config_set(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;
    if (!req->body) return api_error(400, "request body required");

    CUTILS_AUTO_JSON_REQ cutils_json_req_t *body = NULL;
    if (json_req_parse(req->body, req->body_len, &body) != CUTILS_OK)
        return api_error(400, "invalid JSON");

    /* Serialize concurrent writers: BEGIN IMMEDIATE takes RESERVED up
     * front, so the SELECT below observes any committed concurrent
     * write rather than racing it and losing the merge. */
    CUTILS_AUTO_DB_TX cutils_db_tx_t tx = { 0 };
    if (cutils_db_tx_begin_immediate(ctx->db, &tx) != CUTILS_OK)
        return api_error(500, cutils_get_error());

    /* Fetch current config so unspecified fields keep their current values
     * (partial-update semantics). */
    CUTILS_AUTO_DBRES db_result_t *cur = NULL;
    int rc = db_execute(ctx->db,
        "SELECT enabled, latitude, longitude, alert_zones, alert_types, "
        "wind_speed_mph, severe_keywords, poll_interval, "
        "control_register, severe_raw_value, normal_raw_value "
        "FROM weather_config WHERE id = 1", NULL, &cur);
    if (rc != CUTILS_OK || !cur || cur->nrows == 0)
        return api_error(404, "weather config not found");
    char **row = cur->rows[0];

    /* Seed typed locals from DB; each override attempt is a no-op if the
     * field is missing or wrong type (the out-param isn't written on
     * failure), preserving the original "silently ignore bad fields"
     * partial-update behavior. */
    bool    enabled        = atoi(row[0]) != 0;
    double  latitude       = atof(row[1]);
    double  longitude      = atof(row[2]);
    int32_t wind_speed_mph = atoi(row[5]);
    int32_t poll_interval  = atoi(row[7]);
    int32_t severe_raw     = row[9]  ? atoi(row[9])  : 0;
    int32_t normal_raw     = row[10] ? atoi(row[10]) : 0;

    CUTILS_AUTOFREE char *zones_req  = NULL;
    CUTILS_AUTOFREE char *atypes_req = NULL;
    CUTILS_AUTOFREE char *skw_req    = NULL;
    CUTILS_AUTOFREE char *creg_req   = NULL;

    /* Wide ranges preserve the original "accept anything" semantics;
     * tightening validation is a separate concern. */
    CUTILS_UNUSED(json_req_get_bool   (body, "enabled",          &enabled));
    CUTILS_UNUSED(json_req_get_f64    (body, "latitude",         &latitude,       -1e308, 1e308));
    CUTILS_UNUSED(json_req_get_f64    (body, "longitude",        &longitude,      -1e308, 1e308));
    CUTILS_UNUSED(json_req_get_i32    (body, "wind_speed_mph",   &wind_speed_mph, INT32_MIN, INT32_MAX));
    CUTILS_UNUSED(json_req_get_i32    (body, "poll_interval",    &poll_interval,  INT32_MIN, INT32_MAX));
    CUTILS_UNUSED(json_req_get_i32    (body, "severe_raw_value", &severe_raw,     INT32_MIN, INT32_MAX));
    CUTILS_UNUSED(json_req_get_i32    (body, "normal_raw_value", &normal_raw,     INT32_MIN, INT32_MAX));
    CUTILS_UNUSED(json_req_get_str_opt(body, "alert_zones",      &zones_req));
    CUTILS_UNUSED(json_req_get_str_opt(body, "alert_types",      &atypes_req));
    CUTILS_UNUSED(json_req_get_str_opt(body, "severe_keywords",  &skw_req));
    CUTILS_UNUSED(json_req_get_str_opt(body, "control_register", &creg_req));

    const char *zones  = zones_req  ? zones_req  : (row[3] ? row[3] : "");
    const char *atypes = atypes_req ? atypes_req : (row[4] ? row[4] : "");
    const char *skw    = skw_req    ? skw_req    : (row[6] ? row[6] : "");
    const char *creg   = creg_req   ? creg_req   : (row[8] ? row[8] : "freq_tolerance");

    char en_s[4], lat_s[32], lon_s[32], ws_s[16], pi_s[16], srv_s[16], nrv_s[16];
    snprintf(en_s,  sizeof(en_s),  "%d",    enabled ? 1 : 0);
    snprintf(lat_s, sizeof(lat_s), "%.6f",  latitude);
    snprintf(lon_s, sizeof(lon_s), "%.6f",  longitude);
    snprintf(ws_s,  sizeof(ws_s),  "%d",    wind_speed_mph);
    snprintf(pi_s,  sizeof(pi_s),  "%d",    poll_interval);
    snprintf(srv_s, sizeof(srv_s), "%d",    severe_raw);
    snprintf(nrv_s, sizeof(nrv_s), "%d",    normal_raw);

    const char *params[] = {
        en_s, lat_s, lon_s,
        zones, atypes,
        ws_s, skw, pi_s,
        creg, srv_s, nrv_s,
        NULL
    };
    rc = db_execute_non_query(ctx->db,
        "UPDATE weather_config SET enabled=?, latitude=?, longitude=?, "
        "alert_zones=?, alert_types=?, wind_speed_mph=?, "
        "severe_keywords=?, poll_interval=?, "
        "control_register=?, severe_raw_value=?, normal_raw_value=? WHERE id = 1",
        params, NULL);

    if (rc != CUTILS_OK) return api_error(500, "failed to update weather config");

    if (db_tx_commit(&tx) != CUTILS_OK)
        return api_error(500, cutils_get_error());

    CUTILS_AUTO_JSON_RESP cutils_json_resp_t *resp = NULL;
    if (json_resp_new(&resp) != CUTILS_OK)
        return api_error(500, cutils_get_error());
    ADD_OR_FAIL(json_resp_add_str(resp, "result", "updated"));
    ADD_OR_FAIL(json_resp_add_str(resp, "note",   "restart daemon to apply changes"));

    CUTILS_AUTOFREE char *json = NULL;
    size_t len;
    if (json_resp_finalize(resp, &json, &len) != CUTILS_OK)
        return api_error(500, cutils_get_error());
    return api_ok(CUTILS_MOVE(json));
}

static api_response_t handle_weather_simulate(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;
    if (!ctx->weather) return api_error(503, "weather subsystem not running");
    if (!req->body) return api_error(400, "request body required");

    CUTILS_AUTO_JSON_REQ cutils_json_req_t *body = NULL;
    if (json_req_parse(req->body, req->body_len, &body) != CUTILS_OK)
        return api_error(400, "invalid JSON");

    CUTILS_AUTOFREE char *action = NULL;
    CUTILS_UNUSED(json_req_get_str_opt(body, "action", &action));

    if (action && strcmp(action, "severe") == 0) {
        CUTILS_AUTOFREE char *reason = NULL;
        CUTILS_UNUSED(json_req_get_str_opt(body, "reason", &reason));
        weather_simulate_severe(ctx->weather, reason ? reason : "Manual simulation");
        return api_ok_msg("severe weather simulated");
    } else if (action && strcmp(action, "clear") == 0) {
        weather_simulate_clear(ctx->weather);
        return api_ok_msg("simulation cleared");
    }

    return api_error(400, "action must be 'severe' or 'clear'");
}

static api_response_t handle_weather_report(const api_request_t *req, void *ud)
{
    (void)req;
    route_ctx_t *ctx = ud;
    char *json = weather_report_json(ctx->weather);
    return api_ok(json);
}

#undef ADD_OR_FAIL

/* --- Registration --- */

void api_register_weather_routes(api_server_t *srv, route_ctx_t *ctx)
{
    api_server_route(srv, "/api/weather/status",   API_GET,  handle_weather_status,     ctx);
    api_server_route(srv, "/api/weather/config",   API_GET,  handle_weather_config_get, ctx);
    api_server_route(srv, "/api/weather/config",   API_POST, handle_weather_config_set, ctx);
    api_server_route(srv, "/api/weather/simulate", API_POST, handle_weather_simulate,   ctx);
    api_server_route(srv, "/api/weather/report",   API_GET,  handle_weather_report,     ctx);
}
