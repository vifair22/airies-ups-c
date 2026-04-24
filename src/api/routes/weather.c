#include "api/routes/routes.h"
#include <cJSON.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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

    db_result_t *cur = NULL;
    /* Failure manifests as cur == NULL or nrows == 0; the check below
     * handles both identically (respond 404 weather-not-configured). */
    CUTILS_UNUSED(db_execute(ctx->db,
        "SELECT enabled, latitude, longitude, alert_zones, alert_types, "
        "wind_speed_mph, severe_keywords, poll_interval, "
        "control_register, severe_raw_value, normal_raw_value "
        "FROM weather_config WHERE id = 1", NULL, &cur));

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

static api_response_t handle_weather_simulate(const api_request_t *req, void *ud)
{
    route_ctx_t *ctx = ud;
    if (!ctx->weather) return api_error(503, "weather subsystem not running");
    if (!req->body) return api_error(400, "request body required");

    cJSON *body = cJSON_Parse(req->body);
    if (!body) return api_error(400, "invalid JSON");

    const cJSON *jaction = cJSON_GetObjectItem(body, "action");
    const char *action = (jaction && cJSON_IsString(jaction)) ? jaction->valuestring : "";

    if (strcmp(action, "severe") == 0) {
        const cJSON *jreason = cJSON_GetObjectItem(body, "reason");
        const char *reason = (jreason && cJSON_IsString(jreason))
            ? jreason->valuestring : "Manual simulation";
        weather_simulate_severe(ctx->weather, reason);
        cJSON_Delete(body);
        return api_ok_msg("severe weather simulated");
    } else if (strcmp(action, "clear") == 0) {
        weather_simulate_clear(ctx->weather);
        cJSON_Delete(body);
        return api_ok_msg("simulation cleared");
    }

    cJSON_Delete(body);
    return api_error(400, "action must be 'severe' or 'clear'");
}

static api_response_t handle_weather_report(const api_request_t *req, void *ud)
{
    (void)req;
    route_ctx_t *ctx = ud;
    char *json = weather_report_json(ctx->weather);
    return api_ok(json);
}

/* --- Registration --- */

void api_register_weather_routes(api_server_t *srv, route_ctx_t *ctx)
{
    api_server_route(srv, "/api/weather/status",   API_GET,  handle_weather_status,     ctx);
    api_server_route(srv, "/api/weather/config",   API_GET,  handle_weather_config_get, ctx);
    api_server_route(srv, "/api/weather/config",   API_POST, handle_weather_config_set, ctx);
    api_server_route(srv, "/api/weather/simulate", API_POST, handle_weather_simulate,   ctx);
    api_server_route(srv, "/api/weather/report",   API_GET,  handle_weather_report,     ctx);
}
