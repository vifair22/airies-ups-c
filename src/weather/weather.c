#include "weather/weather.h"
#include "ups/ups.h"
#include <cutils/log.h>
#include <cutils/error.h>
#include <cutils/json.h>
#include <cutils/mem.h>

/* weather.c parses NWS responses from api.weather.gov using cJSON and
 * builds our own outgoing responses using cutils/json. The cJSON side
 * is confined to the nws_get helper and the parsing functions below —
 * all string fields extracted from the NWS tree are either used
 * immediately (strstr/atoi compare) or copied via snprintf / owned
 * adders before the tree is deleted, so the borrowed-valuestring UAF
 * class doesn't apply.
 *
 * The cu_json fence (c-utils!8) blocks cJSON.h by default; this file
 * legitimately needs it for the NWS parse helpers above. */
#define CUTILS_CJSON_ALLOW
#include <cJSON.h>

#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <ctype.h>

#define NWS_BASE "https://api.weather.gov"
#define NWS_UA   "(airies-ups-weather, vifair22@gmail.com)"

/* Case-insensitive substring search (strcasestr is a GNU extension) */
static const char *ci_strstr(const char *haystack, const char *needle)
{
    if (!needle[0]) return haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) {
            h++; n++;
        }
        if (!*n) return haystack;
    }
    return NULL;
}

/* --- curl response buffer --- */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} curl_buf_t;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *ud)
{
    curl_buf_t *buf = ud;
    size_t bytes = size * nmemb;
    if (buf->len + bytes + 1 > buf->cap) {
        size_t newcap = (buf->len + bytes + 1) * 2;
        char *tmp = realloc(buf->data, newcap);
        if (!tmp) return 0;
        buf->data = tmp;
        buf->cap = newcap;
    }
    memcpy(buf->data + buf->len, ptr, bytes);
    buf->len += bytes;
    buf->data[buf->len] = '\0';
    return bytes;
}

static cJSON *nws_get(const char *url)
{
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;

    curl_buf_t buf = { .data = malloc(4096), .len = 0, .cap = 4096 };
    if (!buf.data) { curl_easy_cleanup(curl); return NULL; }
    buf.data[0] = '\0';

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/geo+json");
    char ua[128];
    snprintf(ua, sizeof(ua), "User-Agent: %s", NWS_UA);
    headers = curl_slist_append(headers, ua);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) { free(buf.data); return NULL; }

    cJSON *json = cJSON_Parse(buf.data);
    free(buf.data);
    return json;
}

/* --- Weather state --- */

struct weather {
    cutils_db_t *db;
    monitor_t   *monitor;
    ups_t       *ups;
    pthread_t    thread;
    volatile int running;

    /* Config (loaded from DB) */
    int     enabled;
    double  latitude;
    double  longitude;
    char    alert_zones[256];
    char    alert_types[1024];
    int     wind_speed_mph;
    char    severe_keywords[256];
    int     poll_interval;

    /* Generic parameter control */
    char     control_register[64];  /* config register name to control */
    uint16_t severe_raw_value;      /* value to write during severe weather */
    uint16_t normal_raw_value;      /* fallback restore value */

    /* Saved register state for restore */
    uint16_t saved_value;           /* register value before we wrote severe */
    int      has_saved;             /* true if we read and saved a value */

    /* State */
    int     severe;
    char    reasons[512];
    char    forecast_url[256];

    /* Simulation (injected via API, consumed by next poll cycle) */
    volatile int  sim_severe;       /* 1 = inject severe, -1 = force clear, 0 = normal */
    int           sim_active;       /* 1 = current severe state originated from simulation */
    char          sim_reasons[256];
};

static int load_config(weather_t *w)
{
    CUTILS_AUTO_DBRES db_result_t *result = NULL;
    int rc = db_execute(w->db,
        "SELECT enabled, latitude, longitude, alert_zones, alert_types, "
        "wind_speed_mph, severe_keywords, poll_interval, "
        "control_register, severe_raw_value, normal_raw_value "
        "FROM weather_config WHERE id = 1",
        NULL, &result);

    if (rc != 0 || !result || result->nrows == 0) return -1;

    w->enabled = atoi(result->rows[0][0]);
    w->latitude = atof(result->rows[0][1]);
    w->longitude = atof(result->rows[0][2]);
    snprintf(w->alert_zones, sizeof(w->alert_zones), "%s",
             result->rows[0][3] ? result->rows[0][3] : "");
    snprintf(w->alert_types, sizeof(w->alert_types), "%s",
             result->rows[0][4] ? result->rows[0][4] : "");
    w->wind_speed_mph = atoi(result->rows[0][5]);
    snprintf(w->severe_keywords, sizeof(w->severe_keywords), "%s",
             result->rows[0][6] ? result->rows[0][6] : "");
    w->poll_interval = atoi(result->rows[0][7]);
    if (w->poll_interval < 60) w->poll_interval = 300;

    /* Generic param control */
    snprintf(w->control_register, sizeof(w->control_register), "%s",
             result->rows[0][8] ? result->rows[0][8] : "freq_tolerance");
    w->severe_raw_value = result->rows[0][9] ? (uint16_t)atoi(result->rows[0][9]) : 0;
    w->normal_raw_value = result->rows[0][10] ? (uint16_t)atoi(result->rows[0][10]) : 0;

    return 0;
}

/* --- NWS checks --- */

static int check_alerts(weather_t *w, char *reasons, size_t reasons_sz)
{
    if (!w->alert_zones[0]) return 0;

    char url[512];
    snprintf(url, sizeof(url), "%s/alerts/active?zone=%s",
             NWS_BASE, w->alert_zones);

    cJSON *json = nws_get(url);
    if (!json) return 0;

    cJSON *features = cJSON_GetObjectItem(json, "features");
    if (!features || !cJSON_IsArray(features)) {
        cJSON_Delete(json);
        return 0;
    }

    int severe = 0;
    reasons[0] = '\0';
    size_t rpos = 0;

    int nfeatures = cJSON_GetArraySize(features);
    for (int i = 0; i < nfeatures; i++) {
        cJSON *f = cJSON_GetArrayItem(features, i);
        cJSON *props = cJSON_GetObjectItem(f, "properties");
        if (!props) continue;
        cJSON *event = cJSON_GetObjectItem(props, "event");
        if (!event || !cJSON_IsString(event)) continue;

        if (strstr(w->alert_types, event->valuestring)) {
            severe = 1;
            if (rpos > 0 && rpos < reasons_sz - 2)
                rpos += (size_t)snprintf(reasons + rpos, reasons_sz - rpos, "; ");
            rpos += (size_t)snprintf(reasons + rpos, reasons_sz - rpos,
                                     "%s", event->valuestring);
        }
    }

    cJSON_Delete(json);
    return severe;
}

static int check_forecast(weather_t *w, char *reasons, size_t reasons_sz,
                          size_t rpos)
{
    if (!w->forecast_url[0]) return 0;

    cJSON *json = nws_get(w->forecast_url);
    if (!json) return 0;

    cJSON *props = cJSON_GetObjectItem(json, "properties");
    cJSON *periods = props ? cJSON_GetObjectItem(props, "periods") : NULL;
    if (!periods || !cJSON_IsArray(periods)) {
        cJSON_Delete(json);
        return 0;
    }

    int severe = 0;
    int max_wind = 0;

    int nperiods = cJSON_GetArraySize(periods);
    int check = nperiods > 3 ? 3 : nperiods;

    for (int i = 0; i < check; i++) {
        cJSON *period = cJSON_GetArrayItem(periods, i);

        cJSON *wind = cJSON_GetObjectItem(period, "windSpeed");
        if (wind && cJSON_IsString(wind)) {
            const char *ws = wind->valuestring;
            int speed = 0;
            char *to = strstr(ws, " to ");
            if (to) speed = atoi(to + 4);
            else speed = atoi(ws);
            if (speed > max_wind) max_wind = speed;
        }

        cJSON *forecast = cJSON_GetObjectItem(period, "shortForecast");
        if (forecast && cJSON_IsString(forecast) && w->severe_keywords[0]) {
            char kw_buf[256];
            snprintf(kw_buf, sizeof(kw_buf), "%s", w->severe_keywords);
            char *tok = strtok(kw_buf, ",");
            while (tok) {
                while (*tok == ' ') tok++;
                if (ci_strstr(forecast->valuestring, tok)) {
                    severe = 1;
                    if (rpos > 0 && rpos < reasons_sz - 2)
                        rpos += (size_t)snprintf(reasons + rpos, reasons_sz - rpos, "; ");
                    rpos += (size_t)snprintf(reasons + rpos, reasons_sz - rpos,
                                             "Forecast: %s", forecast->valuestring);
                    break;
                }
                tok = strtok(NULL, ",");
            }
        }
    }

    if (max_wind >= w->wind_speed_mph) {
        severe = 1;
        if (rpos > 0 && rpos < reasons_sz - 2)
            rpos += (size_t)snprintf(reasons + rpos, reasons_sz - rpos, "; ");
        snprintf(reasons + rpos, reasons_sz - rpos, "Wind %dmph", max_wind);
    }

    cJSON_Delete(json);
    return severe;
}

static void resolve_gridpoint(weather_t *w)
{
    char url[256];
    snprintf(url, sizeof(url), "%s/points/%.4f,%.4f",
             NWS_BASE, w->latitude, w->longitude);

    cJSON *json = nws_get(url);
    if (!json) return;

    cJSON *props = cJSON_GetObjectItem(json, "properties");
    cJSON *forecast = props ? cJSON_GetObjectItem(props, "forecastHourly") : NULL;
    if (forecast && cJSON_IsString(forecast))
        snprintf(w->forecast_url, sizeof(w->forecast_url),
                 "%s", forecast->valuestring);

    cJSON_Delete(json);
}

/* --- Generic parameter control ---
 *
 * Read/save the current register value, then write the severe or restore value.
 * If raw values aren't configured, fall back to legacy freq setting names. */

static int write_param(weather_t *w, uint16_t value, const char *label)
{
    const ups_config_reg_t *reg = ups_find_config_reg(w->ups, w->control_register);
    if (!reg) {
        log_warn("weather: control register '%s' not found in driver",
                 w->control_register);
        return -1;
    }
    if (!reg->writable) {
        log_warn("weather: control register '%s' is read-only", w->control_register);
        return -1;
    }

    int rc = ups_config_write(w->ups, reg, value);
    if (rc != 0)
        log_error("weather: failed to write %s=%u to %s", label, value,
                  w->control_register);
    else
        log_info("weather: wrote %s=%u to %s", label, value, w->control_register);

    return rc;
}

static int read_and_save_param(weather_t *w)
{
    const ups_config_reg_t *reg = ups_find_config_reg(w->ups, w->control_register);
    if (!reg) return -1;

    uint16_t current = 0;
    int rc = ups_config_read(w->ups, reg, &current, NULL, 0);
    if (rc == 0) {
        w->saved_value = current;
        w->has_saved = 1;
        log_info("weather: saved current %s=%u before override",
                 w->control_register, current);
    }
    return rc;
}

static void apply_severe(weather_t *w)
{
    read_and_save_param(w);
    write_param(w, w->severe_raw_value, "severe");
}

static void apply_restore(weather_t *w)
{
    /* normal_raw_value of 0xFFFF (-1 from the UI) means "restore saved value".
     * Any other value is a fixed restore target. */
    if (w->normal_raw_value == 0xFFFF) {
        if (w->has_saved) {
            write_param(w, w->saved_value, "restore (saved)");
            w->has_saved = 0;
        } else {
            log_warn("weather: restore requested but no saved value available");
        }
    } else {
        write_param(w, w->normal_raw_value, "restore (fixed)");
        w->has_saved = 0;
    }
}

/* --- Thread --- */

static void *weather_thread(void *arg)
{
    weather_t *w = arg;

    /* Resolve gridpoint once */
    if (w->latitude != 0.0 && w->longitude != 0.0) {
        resolve_gridpoint(w);
        if (w->forecast_url[0])
            log_info("weather: NWS forecast endpoint resolved");
        else
            log_warn("weather: failed to resolve NWS gridpoint, "
                     "wind/forecast checks disabled");
    }

    while (w->running) {
        char reasons[512] = "";
        int severe = 0;

        severe |= check_alerts(w, reasons, sizeof(reasons));
        severe |= check_forecast(w, reasons, sizeof(reasons), strlen(reasons));

        /* Simulation override: injected via API */
        if (w->sim_severe == 1) {
            severe = 1;
            snprintf(reasons, sizeof(reasons), "[SIMULATED] %s", w->sim_reasons);
            w->sim_severe = 0; /* one-shot: consumed */
            w->sim_active = 1;
        } else if (w->sim_severe == -1) {
            severe = 0;
            reasons[0] = '\0';
            w->sim_severe = 0;
            w->sim_active = 0;
        } else if (!severe) {
            w->sim_active = 0; /* real NWS cleared it */
        }

        w->severe = severe;
        snprintf(w->reasons, sizeof(w->reasons), "%s", reasons);

        /* Parameter control logic */
        int he_active = monitor_he_inhibit_active(w->monitor);
        const char *he_source = monitor_he_inhibit_source(w->monitor);

        if (severe && !he_active) {
            log_warn("weather: severe conditions (%s), applying override to %s",
                     reasons, w->control_register);
            apply_severe(w);
            monitor_he_inhibit_set(w->monitor, "weather");
            monitor_fire_event(w->monitor, "warning", "weather",
                               "Weather: Severe", reasons);
        } else if (!severe && he_active && strcmp(he_source, "weather") == 0) {
            log_info("weather: conditions cleared, restoring %s",
                     w->control_register);
            apply_restore(w);
            monitor_he_inhibit_clear(w->monitor);
            monitor_fire_event(w->monitor, "info", "weather",
                               "Weather: Clear",
                               "Conditions cleared, parameter restored");
        } else if (severe && he_active && strcmp(he_source, "weather") == 0) {
            log_info("weather: still severe (%s)", reasons);
        } else if (!severe) {
            log_info("weather: all clear");
        }

        for (int i = 0; i < w->poll_interval && w->running; i++)
            sleep(1);
    }

    return NULL;
}

/* --- Public API --- */

weather_t *weather_create(cutils_db_t *db, monitor_t *monitor, ups_t *ups)
{
    weather_t *w = calloc(1, sizeof(*w));
    if (!w) return NULL;

    w->db = db;
    w->monitor = monitor;
    w->ups = ups;

    if (load_config(w) != 0 || !w->enabled) {
        log_info("weather: disabled or not configured");
        free(w);
        return NULL;
    }

    log_info("weather: zones=%s wind=%dmph poll=%ds control=%s (severe=%u, normal=%u)",
             w->alert_zones, w->wind_speed_mph, w->poll_interval,
             w->control_register, w->severe_raw_value, w->normal_raw_value);

    return w;
}

int weather_start(weather_t *w)
{
    w->running = 1;
    int rc = pthread_create(&w->thread, NULL, weather_thread, w);
    if (rc != 0) {
        w->running = 0;
        return -1;
    }
    return 0;
}

void weather_stop(weather_t *w)
{
    if (!w) return;
    w->running = 0;
    pthread_join(w->thread, NULL);
    free(w);
}

char *weather_status_json(weather_t *w)
{
    CUTILS_AUTO_JSON_RESP cutils_json_resp_t *resp = NULL;
    CUTILS_AUTOFREE       char               *json = NULL;
    size_t len;

    if (json_resp_new(&resp) != CUTILS_OK) return NULL;

    if (!w) {
        if (json_resp_add_bool(resp, "enabled", false) != CUTILS_OK ||
            json_resp_finalize(resp, &json, &len) != CUTILS_OK)
            return NULL;
        return CUTILS_MOVE(json);
    }

    if (json_resp_add_bool(resp, "enabled",            true)               != CUTILS_OK ||
        json_resp_add_bool(resp, "severe",             w->severe != 0)     != CUTILS_OK ||
        json_resp_add_str (resp, "reasons",            w->reasons)         != CUTILS_OK ||
        json_resp_add_f64 (resp, "latitude",           w->latitude)        != CUTILS_OK ||
        json_resp_add_f64 (resp, "longitude",          w->longitude)       != CUTILS_OK ||
        json_resp_add_str (resp, "alert_zones",        w->alert_zones)     != CUTILS_OK ||
        json_resp_add_i32 (resp, "wind_threshold_mph", w->wind_speed_mph)  != CUTILS_OK ||
        json_resp_add_i32 (resp, "poll_interval",      w->poll_interval)   != CUTILS_OK ||
        json_resp_add_str (resp, "control_register",   w->control_register)!= CUTILS_OK ||
        json_resp_add_bool(resp, "simulated",          w->sim_active != 0) != CUTILS_OK ||
        json_resp_finalize(resp, &json, &len)                              != CUTILS_OK) {
        return NULL;
    }
    return CUTILS_MOVE(json);
}

void weather_simulate_severe(weather_t *w, const char *reason)
{
    if (!w) return;
    snprintf(w->sim_reasons, sizeof(w->sim_reasons), "%s",
             reason ? reason : "Manual simulation");
    w->sim_severe = 1;
    log_info("weather: severe event simulated — %s", w->sim_reasons);
}

void weather_simulate_clear(weather_t *w)
{
    if (!w) return;
    w->sim_severe = -1;
    log_info("weather: simulation cleared, will restore on next cycle");
}

/* Copy a cJSON string field (if present and correct type) into the
 * current element at the given key. Silently skips missing/wrong-type
 * to preserve the original's best-effort behavior. */
static int elem_add_str_from_cjson(cutils_json_elem_t *elem,
                                   const cJSON *src, const char *src_key,
                                   const char *dst_key)
{
    const cJSON *item = cJSON_GetObjectItem(src, src_key);
    if (item && cJSON_IsString(item))
        return json_elem_add_str(elem, dst_key, item->valuestring);
    return CUTILS_OK;
}

char *weather_report_json(weather_t *w)
{
    CUTILS_AUTO_JSON_RESP cutils_json_resp_t *resp = NULL;
    CUTILS_AUTOFREE       char               *json = NULL;
    size_t len;

    if (json_resp_new(&resp) != CUTILS_OK) return NULL;

    if (!w) {
        if (json_resp_add_str(resp, "error", "weather subsystem not running") != CUTILS_OK ||
            json_resp_finalize(resp, &json, &len) != CUTILS_OK)
            return NULL;
        return CUTILS_MOVE(json);
    }

    /* Always emit both arrays, even if empty. */
    if (json_resp_ensure_array(resp, "alerts")   != CUTILS_OK ||
        json_resp_ensure_array(resp, "forecast") != CUTILS_OK)
        return NULL;

    /* Fetch active alerts — parse NWS with cJSON, build our response
     * element-by-element via cu_json. Every string adder copies the
     * source, so the cJSON tree can be freed as soon as we're done. */
    if (w->alert_zones[0]) {
        char url[512];
        snprintf(url, sizeof(url), "%s/alerts/active?zone=%s",
                 NWS_BASE, w->alert_zones);
        cJSON *nws = nws_get(url);
        if (nws) {
            cJSON *features = cJSON_GetObjectItem(nws, "features");
            if (features && cJSON_IsArray(features)) {
                int n = cJSON_GetArraySize(features);
                for (int i = 0; i < n; i++) {
                    cJSON *f = cJSON_GetArrayItem(features, i);
                    cJSON *props = cJSON_GetObjectItem(f, "properties");
                    if (!props) continue;

                    CUTILS_AUTO_JSON_ELEM cutils_json_elem_t alert;
                    if (json_resp_array_append_begin(resp, "alerts", &alert) != CUTILS_OK) {
                        cJSON_Delete(nws);
                        return NULL;
                    }
                    CUTILS_UNUSED(elem_add_str_from_cjson(&alert, props, "event",    "event"));
                    CUTILS_UNUSED(elem_add_str_from_cjson(&alert, props, "headline", "headline"));
                    CUTILS_UNUSED(elem_add_str_from_cjson(&alert, props, "severity", "severity"));
                    CUTILS_UNUSED(elem_add_str_from_cjson(&alert, props, "urgency",  "urgency"));

                    const cJSON *ev = cJSON_GetObjectItem(props, "event");
                    bool matched = (ev && cJSON_IsString(ev) &&
                                    strstr(w->alert_types, ev->valuestring)) ? true : false;
                    CUTILS_UNUSED(json_elem_add_bool(&alert, "matched", matched));

                    json_elem_commit(&alert);
                }
            }
            cJSON_Delete(nws);
        }
    }

    /* Fetch forecast periods. */
    if (w->forecast_url[0]) {
        cJSON *nws = nws_get(w->forecast_url);
        if (nws) {
            cJSON *props = cJSON_GetObjectItem(nws, "properties");
            cJSON *periods = props ? cJSON_GetObjectItem(props, "periods") : NULL;
            if (periods && cJSON_IsArray(periods)) {
                int n = cJSON_GetArraySize(periods);
                int show = n > 6 ? 6 : n;
                for (int i = 0; i < show; i++) {
                    cJSON *p = cJSON_GetArrayItem(periods, i);

                    CUTILS_AUTO_JSON_ELEM cutils_json_elem_t period;
                    if (json_resp_array_append_begin(resp, "forecast", &period) != CUTILS_OK) {
                        cJSON_Delete(nws);
                        return NULL;
                    }
                    CUTILS_UNUSED(elem_add_str_from_cjson(&period, p, "name",             "name"));

                    const cJSON *temp = cJSON_GetObjectItem(p, "temperature");
                    if (temp && cJSON_IsNumber(temp))
                        CUTILS_UNUSED(json_elem_add_f64(&period, "temperature", temp->valuedouble));

                    CUTILS_UNUSED(elem_add_str_from_cjson(&period, p, "windSpeed",        "wind"));
                    CUTILS_UNUSED(elem_add_str_from_cjson(&period, p, "windDirection",    "wind_direction"));
                    CUTILS_UNUSED(elem_add_str_from_cjson(&period, p, "shortForecast",    "short_forecast"));
                    CUTILS_UNUSED(elem_add_str_from_cjson(&period, p, "detailedForecast", "detailed_forecast"));

                    json_elem_commit(&period);
                }
            }
            cJSON_Delete(nws);
        }
    }

    if (json_resp_finalize(resp, &json, &len) != CUTILS_OK) return NULL;
    return CUTILS_MOVE(json);
}
