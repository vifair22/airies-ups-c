#include "weather/weather.h"
#include "ups/ups.h"
#include <cutils/log.h>
#include <cutils/push.h>
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

    /* State */
    int     severe;
    char    reasons[512];
    char    forecast_url[256];
};

static int load_config(weather_t *w)
{
    db_result_t *result = NULL;
    int rc = db_execute(w->db,
        "SELECT enabled, latitude, longitude, alert_zones, alert_types, "
        "wind_speed_mph, severe_keywords, poll_interval "
        "FROM weather_config WHERE id = 1",
        NULL, &result);

    if (rc != 0 || !result || result->nrows == 0) {
        db_result_free(result);
        return -1;
    }

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

    db_result_free(result);
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

        /* Check if this event type is in our alert_types list */
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

    /* Check first 3 hours */
    int nperiods = cJSON_GetArraySize(periods);
    int check = nperiods > 3 ? 3 : nperiods;

    for (int i = 0; i < check; i++) {
        cJSON *period = cJSON_GetArrayItem(periods, i);

        /* Wind speed */
        cJSON *wind = cJSON_GetObjectItem(period, "windSpeed");
        if (wind && cJSON_IsString(wind)) {
            /* Parse "15 mph" or "15 to 25 mph" */
            const char *ws = wind->valuestring;
            int speed = 0;
            char *to = strstr(ws, " to ");
            if (to) speed = atoi(to + 4);
            else speed = atoi(ws);
            if (speed > max_wind) max_wind = speed;
        }

        /* Severe keywords */
        cJSON *forecast = cJSON_GetObjectItem(period, "shortForecast");
        if (forecast && cJSON_IsString(forecast) && w->severe_keywords[0]) {
            /* Check each keyword (comma-separated) */
            char kw_buf[256];
            snprintf(kw_buf, sizeof(kw_buf), "%s", w->severe_keywords);
            char *tok = strtok(kw_buf, ",");
            while (tok) {
                while (*tok == ' ') tok++;
                /* Case-insensitive search */
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

/* --- Event journal helper --- */

static void weather_event(weather_t *w, const char *severity,
                          const char *title, const char *message)
{
    char ts[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm);

    const char *params[] = { ts, severity, "weather", title, message, NULL };
    db_execute_non_query(w->db,
        "INSERT INTO events (timestamp, severity, category, title, message) "
        "VALUES (?, ?, ?, ?, ?)",
        params, NULL);
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

        /* Check alerts */
        severe |= check_alerts(w, reasons, sizeof(reasons));

        /* Check forecast + wind */
        severe |= check_forecast(w, reasons, sizeof(reasons), strlen(reasons));

        /* Update state */
        w->severe = severe;
        snprintf(w->reasons, sizeof(w->reasons), "%s", reasons);

        /* HE inhibit logic */
        int he_active = monitor_he_inhibit_active(w->monitor);
        const char *he_source = monitor_he_inhibit_source(w->monitor);

        if (severe && !he_active) {
            log_warn("weather: severe conditions (%s), inhibiting HE", reasons);
            const ups_freq_setting_t *fs = ups_find_freq_setting(w->ups, "hz60_0_1");
            if (fs) {
                ups_cmd_set_freq_tolerance(w->ups, fs->value);
                monitor_he_inhibit_set(w->monitor, "weather");
            }
            weather_event(w, "warning", "Weather Inhibit", reasons);
            push_send("UPS Weather Inhibit", reasons);
        } else if (!severe && he_active && strcmp(he_source, "weather") == 0) {
            log_info("weather: conditions cleared, restoring HE");
            const ups_freq_setting_t *fs = ups_find_freq_setting(w->ups, "hz60_3_0");
            if (fs) {
                ups_cmd_set_freq_tolerance(w->ups, fs->value);
                monitor_he_inhibit_clear(w->monitor);
            }
            weather_event(w, "info", "Weather Clear",
                          "Conditions cleared, HE mode restored");
            push_send("UPS Weather Clear",
                       "Conditions cleared, HE mode restored");
        } else if (severe && he_active && strcmp(he_source, "weather") == 0) {
            log_info("weather: still severe (%s)", reasons);
        } else if (!severe) {
            log_info("weather: all clear");
        }

        /* Sleep in 1s increments so we can stop quickly */
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

    log_info("weather: zones=%s wind=%dmph poll=%ds",
             w->alert_zones, w->wind_speed_mph, w->poll_interval);

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
    cJSON *obj = cJSON_CreateObject();
    if (!w) {
        cJSON_AddBoolToObject(obj, "enabled", 0);
        char *json = cJSON_PrintUnformatted(obj);
        cJSON_Delete(obj);
        return json;
    }

    cJSON_AddBoolToObject(obj, "enabled", 1);
    cJSON_AddBoolToObject(obj, "severe", w->severe);
    cJSON_AddStringToObject(obj, "reasons", w->reasons);
    cJSON_AddNumberToObject(obj, "latitude", w->latitude);
    cJSON_AddNumberToObject(obj, "longitude", w->longitude);
    cJSON_AddStringToObject(obj, "alert_zones", w->alert_zones);
    cJSON_AddNumberToObject(obj, "wind_threshold_mph", w->wind_speed_mph);
    cJSON_AddNumberToObject(obj, "poll_interval", w->poll_interval);

    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    return json;
}
