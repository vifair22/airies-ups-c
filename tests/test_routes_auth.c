/* Route-handler unit tests for src/api/routes/auth_routes.c.
 *
 * The handlers take api_request_t* + a void* userdata (route_ctx_t in
 * production), and return api_response_t. They're directly callable
 * from a test without an MHD instance — we construct the request and
 * context as plain structs.
 *
 * Setup mirrors test_shutdown.c: minimal YAML on disk, a temp SQLite
 * DB, c-utils library migrations + the cookie-auth migration applied
 * inline (we don't run the embedded-migrations runner here), config
 * attached to the DB. */

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <curl/curl.h>

#include <cutils/db.h>
#include <cutils/config.h>
#include <cutils/error.h>

#include "api/auth.h"
#include "api/routes/routes.h"
#include "config/app_config.h"

/* Linker stubs — auth_routes.c calls into ups.c (handle_setup_test) and
 * monitor.c (handle_setup_status) but we don't exercise those handlers
 * here. Stub out their symbols so the binary links without dragging in
 * the full ups + monitor modules and inflating the coverage denominator
 * with code that has nothing to do with the auth handlers under test. */

int   ups_connect    (const ups_conn_params_t *p, ups_t **out)
{ (void)p; (void)out; return -1; }
void  ups_close      (ups_t *u)              { (void)u; }
ups_topology_t ups_topology(const ups_t *u)
{ (void)u; return UPS_TOPO_LINE_INTERACTIVE; }
const char *ups_driver_name(const ups_t *u)  { (void)u; return "stub"; }

int monitor_is_connected(monitor_t *mon)     { (void)mon; return 0; }

#define TEST_YAML "/tmp/airies_test_routes_auth.yaml"
#define TEST_DB   "/tmp/airies_test_routes_auth.db"

/* App schema — combined from migrations/006_auth.sql, 002_events.sql,
 * and 016_sessions_v2.sql. db_run_lib_migrations only runs the c-utils
 * library migrations (config table, etc.) — app tables have to be
 * recreated here. */
static const char *APP_SCHEMA =
    "CREATE TABLE IF NOT EXISTS auth ("
    "  id            INTEGER PRIMARY KEY CHECK (id = 1),"
    "  password_hash TEXT    NOT NULL,"
    "  created_at    TEXT    NOT NULL DEFAULT (datetime('now')),"
    "  updated_at    TEXT    NOT NULL DEFAULT (datetime('now'))"
    ");"
    "CREATE TABLE IF NOT EXISTS events ("
    "  id        INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  timestamp TEXT    NOT NULL,"
    "  severity  TEXT    NOT NULL,"
    "  category  TEXT    NOT NULL,"
    "  title     TEXT    NOT NULL,"
    "  message   TEXT    NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS sessions ("
    "  token        TEXT PRIMARY KEY,"
    "  user_id      INTEGER,"
    "  kind         TEXT NOT NULL DEFAULT 'session',"
    "  name         TEXT,"
    "  scopes       TEXT,"
    "  created_at   TEXT NOT NULL DEFAULT (datetime('now')),"
    "  last_used_at TEXT,"
    "  expires_at   TEXT NOT NULL,"
    "  revoked_at   TEXT"
    ");";

typedef struct {
    cutils_db_t     *db;
    cutils_config_t *cfg;
    route_ctx_t      ctx;
} test_state_t;

static int setup(void **state)
{
    /* Minimal YAML the c-utils config loader needs */
    FILE *f = fopen(TEST_YAML, "w");
    if (!f) return -1;
    fprintf(f, "db:\n  path: %s\n", TEST_DB);
    fclose(f);

    cutils_config_t *cfg = NULL;
    if (config_init(&cfg, "airies_ups", TEST_YAML,
                    CFG_FIRST_RUN_CONTINUE,
                    app_file_keys, app_sections) != CUTILS_OK || !cfg)
        return -1;

    cutils_db_t *db = NULL;
    if (db_open(&db, TEST_DB) != CUTILS_OK || !db) {
        config_free(cfg);
        return -1;
    }

    if (db_run_lib_migrations(db) != CUTILS_OK ||
        db_exec_raw(db, APP_SCHEMA) != CUTILS_OK) {
        db_close(db); config_free(cfg);
        return -1;
    }

    if (config_attach_db(cfg, db, app_db_keys) != CUTILS_OK) {
        db_close(db); config_free(cfg);
        return -1;
    }

    test_state_t *s = calloc(1, sizeof(*s));
    s->db  = db;
    s->cfg = cfg;
    s->ctx.db     = db;
    s->ctx.config = cfg;
    *state = s;
    return 0;
}

static int teardown(void **state)
{
    test_state_t *s = *state;
    db_close(s->db);
    config_free(s->cfg);
    free(s);
    remove(TEST_YAML);
    remove(TEST_DB);
    remove(TEST_DB "-wal");
    remove(TEST_DB "-shm");
    return 0;
}

/* --- Helpers --- */

static api_request_t make_req(const char *body, const char *auth_token)
{
    api_request_t req = {0};
    req.method     = API_POST;
    req.body       = body;
    req.body_len   = body ? strlen(body) : 0;
    req.auth_token = auth_token;
    return req;
}

static void free_resp(api_response_t *r)
{
    free(r->body);
    free(r->set_cookie);
}

/* --- handle_auth_setup --- */

static void test_setup_first_run_succeeds(void **state)
{
    test_state_t *s = *state;
    api_request_t req = make_req("{\"password\":\"hunter2\"}", NULL);
    api_response_t r = handle_auth_setup(&req, &s->ctx);
    assert_int_equal(r.status, 200);
    assert_non_null(strstr(r.body, "password set"));
    assert_int_equal(auth_is_setup(s->db), 1);
    free_resp(&r);
}

static void test_setup_already_set_rejected(void **state)
{
    test_state_t *s = *state;
    auth_set_password(s->db, "existing");
    api_request_t req = make_req("{\"password\":\"hunter2\"}", NULL);
    api_response_t r = handle_auth_setup(&req, &s->ctx);
    assert_int_equal(r.status, 400);
    free_resp(&r);
}

static void test_setup_short_password_rejected(void **state)
{
    test_state_t *s = *state;
    api_request_t req = make_req("{\"password\":\"abc\"}", NULL);
    api_response_t r = handle_auth_setup(&req, &s->ctx);
    assert_int_equal(r.status, 400);
    free_resp(&r);
}

static void test_setup_missing_body_rejected(void **state)
{
    test_state_t *s = *state;
    api_request_t req = make_req(NULL, NULL);
    api_response_t r = handle_auth_setup(&req, &s->ctx);
    assert_int_equal(r.status, 400);
    free_resp(&r);
}

static void test_setup_invalid_json_rejected(void **state)
{
    test_state_t *s = *state;
    api_request_t req = make_req("not json {{", NULL);
    api_response_t r = handle_auth_setup(&req, &s->ctx);
    assert_int_equal(r.status, 400);
    free_resp(&r);
}

/* --- handle_auth_login --- */

static void test_login_correct_password_sets_cookie(void **state)
{
    test_state_t *s = *state;
    auth_set_password(s->db, "hunter2");

    api_request_t req = make_req("{\"password\":\"hunter2\"}", NULL);
    api_response_t r = handle_auth_login(&req, &s->ctx);

    assert_int_equal(r.status, 200);
    assert_non_null(r.set_cookie);
    /* Cookie attributes: HttpOnly + SameSite=Strict + Path=/ + Max-Age,
     * no Secure (cookie_secure default 0) */
    assert_non_null(strstr(r.set_cookie, "auth="));
    assert_non_null(strstr(r.set_cookie, "HttpOnly"));
    assert_non_null(strstr(r.set_cookie, "SameSite=Strict"));
    assert_non_null(strstr(r.set_cookie, "Path=/"));
    assert_non_null(strstr(r.set_cookie, "Max-Age="));
    assert_null(strstr(r.set_cookie, "Secure"));
    /* Body still includes the token for non-browser clients */
    assert_non_null(strstr(r.body, "\"token\""));
    free_resp(&r);
}

static void test_login_secure_flag_when_configured(void **state)
{
    test_state_t *s = *state;
    auth_set_password(s->db, "hunter2");
    assert_int_equal(config_set_db(s->cfg, "auth.cookie_secure", "1"), CUTILS_OK);

    api_request_t req = make_req("{\"password\":\"hunter2\"}", NULL);
    api_response_t r = handle_auth_login(&req, &s->ctx);

    assert_int_equal(r.status, 200);
    assert_non_null(r.set_cookie);
    assert_non_null(strstr(r.set_cookie, "Secure"));
    free_resp(&r);
}

static void test_login_wrong_password_no_cookie(void **state)
{
    test_state_t *s = *state;
    auth_set_password(s->db, "hunter2");

    api_request_t req = make_req("{\"password\":\"wrong\"}", NULL);
    api_response_t r = handle_auth_login(&req, &s->ctx);

    assert_int_equal(r.status, 401);
    assert_null(r.set_cookie);
    free_resp(&r);
}

static void test_login_missing_password_field(void **state)
{
    test_state_t *s = *state;
    auth_set_password(s->db, "hunter2");

    api_request_t req = make_req("{\"not_password\":\"x\"}", NULL);
    api_response_t r = handle_auth_login(&req, &s->ctx);

    assert_int_equal(r.status, 400);
    free_resp(&r);
}

static void test_login_missing_body_rejected(void **state)
{
    test_state_t *s = *state;
    auth_set_password(s->db, "hunter2");

    api_request_t req = make_req(NULL, NULL);
    api_response_t r = handle_auth_login(&req, &s->ctx);

    assert_int_equal(r.status, 400);
    free_resp(&r);
}

/* --- handle_auth_logout --- */

static void test_logout_clears_cookie_and_revokes(void **state)
{
    test_state_t *s = *state;
    auth_set_password(s->db, "hunter2");

    /* Establish a session via login */
    api_request_t login_req = make_req("{\"password\":\"hunter2\"}", NULL);
    api_response_t login_r = handle_auth_login(&login_req, &s->ctx);
    assert_int_equal(login_r.status, 200);

    /* Extract the token from the JSON body for the logout call */
    const char *p = strstr(login_r.body, "\"token\"");
    assert_non_null(p);
    p = strchr(p, ':'); assert_non_null(p);
    p = strchr(p, '"'); assert_non_null(p); p++;
    const char *end = strchr(p, '"');
    assert_non_null(end);
    char token[128];
    assert_true((size_t)(end - p) < sizeof(token));
    memcpy(token, p, (size_t)(end - p));
    token[end - p] = '\0';

    /* Token validates before logout */
    assert_int_equal(auth_validate_token(s->db, token), 1);

    /* Logout via Authorization header */
    char bearer[256];
    snprintf(bearer, sizeof(bearer), "Bearer %s", token);
    api_request_t logout_req = make_req(NULL, bearer);
    api_response_t logout_r = handle_auth_logout(&logout_req, &s->ctx);

    assert_int_equal(logout_r.status, 200);
    assert_non_null(logout_r.set_cookie);
    assert_non_null(strstr(logout_r.set_cookie, "Max-Age=0"));
    assert_non_null(strstr(logout_r.set_cookie, "HttpOnly"));

    /* Token no longer validates */
    assert_int_equal(auth_validate_token(s->db, token), 0);

    free_resp(&login_r);
    free_resp(&logout_r);
}

static void test_logout_without_token_still_clears_cookie(void **state)
{
    test_state_t *s = *state;
    /* Even with no token (e.g. session already gone), logout returns
     * 200 and emits the Set-Cookie clear so the client cleans up. */
    api_request_t req = make_req(NULL, NULL);
    api_response_t r = handle_auth_logout(&req, &s->ctx);

    assert_int_equal(r.status, 200);
    assert_non_null(r.set_cookie);
    assert_non_null(strstr(r.set_cookie, "Max-Age=0"));
    free_resp(&r);
}

/* --- handle_auth_check --- */

static void test_check_returns_ok(void **state)
{
    test_state_t *s = *state;
    /* The auth middleware would have already gated this — by the time
     * the handler runs, the request is authorized. The body just
     * confirms reach. */
    api_request_t req = make_req(NULL, NULL);
    api_response_t r = handle_auth_check(&req, &s->ctx);
    assert_int_equal(r.status, 200);
    assert_non_null(strstr(r.body, "\"ok\""));
    free_resp(&r);
}

/* --- handle_auth_change --- */

static void test_change_correct_old_password(void **state)
{
    test_state_t *s = *state;
    auth_set_password(s->db, "hunter2");

    api_request_t req = make_req(
        "{\"old_password\":\"hunter2\",\"new_password\":\"newpass\"}", NULL);
    api_response_t r = handle_auth_change(&req, &s->ctx);
    assert_int_equal(r.status, 200);
    assert_int_equal(auth_verify_password(s->db, "newpass"), 1);
    free_resp(&r);
}

static void test_change_wrong_old_password(void **state)
{
    test_state_t *s = *state;
    auth_set_password(s->db, "hunter2");

    api_request_t req = make_req(
        "{\"old_password\":\"wrong\",\"new_password\":\"newpass\"}", NULL);
    api_response_t r = handle_auth_change(&req, &s->ctx);
    assert_int_equal(r.status, 401);
    /* Original password unchanged */
    assert_int_equal(auth_verify_password(s->db, "hunter2"), 1);
    free_resp(&r);
}

static void test_change_short_new_password(void **state)
{
    test_state_t *s = *state;
    auth_set_password(s->db, "hunter2");

    api_request_t req = make_req(
        "{\"old_password\":\"hunter2\",\"new_password\":\"ab\"}", NULL);
    api_response_t r = handle_auth_change(&req, &s->ctx);
    assert_int_equal(r.status, 400);
    free_resp(&r);
}

static void test_change_missing_fields(void **state)
{
    test_state_t *s = *state;
    auth_set_password(s->db, "hunter2");

    api_request_t req = make_req("{\"old_password\":\"hunter2\"}", NULL);
    api_response_t r = handle_auth_change(&req, &s->ctx);
    assert_int_equal(r.status, 400);
    free_resp(&r);
}

/* --- handle_setup_status --- */

static void test_setup_status_fresh_install(void **state)
{
    test_state_t *s = *state;
    /* No password, no UPS done — needs_setup=true, password_set=false,
     * ups_configured=false */
    api_request_t req = make_req(NULL, NULL);
    api_response_t r = handle_setup_status(&req, &s->ctx);
    assert_int_equal(r.status, 200);
    assert_non_null(strstr(r.body, "\"needs_setup\""));
    assert_non_null(strstr(r.body, "\"password_set\""));
    assert_non_null(strstr(r.body, "\"ups_configured\""));
    assert_non_null(strstr(r.body, "\"ups_connected\""));
    free_resp(&r);
}

static void test_setup_status_password_set(void **state)
{
    test_state_t *s = *state;
    auth_set_password(s->db, "hunter2");

    api_request_t req = make_req(NULL, NULL);
    api_response_t r = handle_setup_status(&req, &s->ctx);
    assert_int_equal(r.status, 200);
    free_resp(&r);
}

static void test_setup_status_fully_configured(void **state)
{
    test_state_t *s = *state;
    auth_set_password(s->db, "hunter2");
    assert_int_equal(config_set_db(s->cfg, "setup.ups_done", "1"), CUTILS_OK);

    api_request_t req = make_req(NULL, NULL);
    api_response_t r = handle_setup_status(&req, &s->ctx);
    assert_int_equal(r.status, 200);
    free_resp(&r);
}

/* --- handle_setup_ports ---
 *
 * Smoke test only — the handler scans /dev and /sys/class/hidraw on the
 * host running the test. We just verify it returns a 200 with the
 * expected JSON shape. */
static void test_setup_ports_returns_arrays(void **state)
{
    test_state_t *s = *state;
    api_request_t req = make_req(NULL, NULL);
    api_response_t r = handle_setup_ports(&req, &s->ctx);
    assert_int_equal(r.status, 200);
    assert_non_null(strstr(r.body, "\"serial\""));
    assert_non_null(strstr(r.body, "\"usb\""));
    free_resp(&r);
}

/* --- handle_setup_test ---
 *
 * The ups_connect stub above always returns -1, so the handler always
 * hits the failure path. That's enough to exercise the body parsing,
 * params construction, and error response. The success path requires a
 * real UPS connection, which is integration-test territory. */

static void test_setup_test_serial_failure(void **state)
{
    test_state_t *s = *state;
    api_request_t req = make_req(
        "{\"conn_type\":\"serial\",\"device\":\"/dev/ttyUSB0\","
        "\"baud\":9600,\"slave_id\":1}", NULL);
    api_response_t r = handle_setup_test(&req, &s->ctx);
    assert_int_equal(r.status, 502);
    free_resp(&r);
}

static void test_setup_test_usb_failure(void **state)
{
    test_state_t *s = *state;
    api_request_t req = make_req(
        "{\"conn_type\":\"usb\",\"usb_vid\":\"051d\",\"usb_pid\":\"0002\"}",
        NULL);
    api_response_t r = handle_setup_test(&req, &s->ctx);
    assert_int_equal(r.status, 502);
    free_resp(&r);
}

static void test_setup_test_default_serial_missing_device(void **state)
{
    test_state_t *s = *state;
    /* No conn_type → defaults to serial; no device → 400 */
    api_request_t req = make_req("{}", NULL);
    api_response_t r = handle_setup_test(&req, &s->ctx);
    assert_int_equal(r.status, 400);
    free_resp(&r);
}

static void test_setup_test_missing_body(void **state)
{
    test_state_t *s = *state;
    api_request_t req = make_req(NULL, NULL);
    api_response_t r = handle_setup_test(&req, &s->ctx);
    assert_int_equal(r.status, 400);
    free_resp(&r);
}

/* --- api_response builders (no MHD, no DB) --- */

static api_response_t fake_handler(const api_request_t *req, void *ud)
{
    (void)req; (void)ud;
    return api_ok(strdup("{}"));
}

static int fake_auth(const api_request_t *req, const char *url, void *ud)
{
    (void)req; (void)url; (void)ud;
    return 1;
}

static void test_api_ok(void **state)
{
    (void)state;
    api_response_t r = api_ok(strdup("{\"x\":1}"));
    assert_int_equal(r.status, 200);
    assert_string_equal(r.body, "{\"x\":1}");
    assert_string_equal(r.content_type, "application/json");
    free_resp(&r);
}

static void test_api_ok_status(void **state)
{
    (void)state;
    api_response_t r = api_ok_status(201, strdup("{}"));
    assert_int_equal(r.status, 201);
    free_resp(&r);
}

static void test_api_error_includes_message(void **state)
{
    (void)state;
    api_response_t r = api_error(400, "bad input");
    assert_int_equal(r.status, 400);
    assert_non_null(strstr(r.body, "bad input"));
    assert_non_null(strstr(r.body, "\"error\""));
    free_resp(&r);
}

static void test_api_error_null_message(void **state)
{
    (void)state;
    api_response_t r = api_error(500, NULL);
    assert_int_equal(r.status, 500);
    assert_non_null(r.body);
    free_resp(&r);
}

static void test_api_no_content(void **state)
{
    (void)state;
    api_response_t r = api_no_content();
    assert_int_equal(r.status, 204);
    assert_null(r.body);
    /* No free needed — body is NULL */
}

/* --- api_server_create / route / auth / stop ---
 *
 * api_server_create skips daemon allocation when both tcp_port=0 and
 * socket_path=NULL — that gives us a usable server handle for
 * exercising the route table and auth-callback hooks without needing
 * a live MHD instance. */

static void test_server_create_no_listeners(void **state)
{
    (void)state;
    api_server_t *srv = api_server_create(0, NULL, NULL);
    assert_non_null(srv);
    /* api_server_start is a no-op when daemons are already running
     * from create — exercising it for coverage. */
    assert_int_equal(api_server_start(srv), 0);
    api_server_stop(srv);
}

static void test_server_route_registration(void **state)
{
    (void)state;
    api_server_t *srv = api_server_create(0, NULL, NULL);
    assert_non_null(srv);
    /* Register a handful of distinct routes — covers route_entry_t
     * population and the bounds check. */
    assert_int_equal(api_server_route(srv, "/api/x",   API_GET,  fake_handler, NULL), 0);
    assert_int_equal(api_server_route(srv, "/api/y",   API_POST, fake_handler, NULL), 0);
    assert_int_equal(api_server_route(srv, "/api/z*",  API_GET,  fake_handler, NULL), 0);
    api_server_stop(srv);
}

static void test_server_set_auth_callback(void **state)
{
    (void)state;
    api_server_t *srv = api_server_create(0, NULL, NULL);
    assert_non_null(srv);
    /* set_auth has no return; we just exercise the path. */
    api_server_set_auth(srv, fake_auth, NULL);
    api_server_stop(srv);
}

static void test_server_stop_null_safe(void **state)
{
    (void)state;
    /* api_server_stop must tolerate NULL — the create-failed cleanup
     * path relies on it. */
    api_server_stop(NULL);
}

/* --- Integration tests: real MHD daemon + libcurl ---
 *
 * api_server_create spawns MHD's internal polling threads when given a
 * real TCP port; we hit them with libcurl from the test thread. This is
 * what exercises request_handler / send_response / route dispatch /
 * the auth callback chain — none of which are reachable from pure
 * struct-in-struct-out unit tests.
 *
 * Port 18080 is hardcoded for simplicity; if it's in use the MHD
 * create returns NULL and the test skips. */

/* Avoid 18080 — common in dev environments. 48000 is in the IANA
 * dynamic/ephemeral range and unlikely to clash. */
#define TEST_PORT     48091
#define TEST_BASE_URL "http://127.0.0.1:48091"

typedef struct {
    char  *data;
    size_t len;
} curl_buf_t;

static size_t curl_write(char *ptr, size_t size, size_t nmemb, void *ud)
{
    size_t bytes = size * nmemb;
    curl_buf_t *b = ud;
    char *grown = realloc(b->data, b->len + bytes + 1);
    if (!grown) return 0;
    b->data = grown;
    memcpy(b->data + b->len, ptr, bytes);
    b->len += bytes;
    b->data[b->len] = '\0';
    return bytes;
}

static api_response_t int_handler_ok(const api_request_t *req, void *ud)
{
    (void)req; (void)ud;
    return api_ok(strdup("{\"hello\":\"world\"}"));
}

static api_response_t int_handler_echo_query(const api_request_t *req, void *ud)
{
    (void)ud;
    const char *q = api_query_param(req, "name");
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"name\":\"%s\"}", q ? q : "");
    return api_ok(strdup(buf));
}

static int int_auth_deny_protected(const api_request_t *req, const char *url, void *ud)
{
    (void)req; (void)ud;
    /* Allow only /api/public; deny everything else */
    return strcmp(url, "/api/public") == 0;
}

/* Spin up a server with one or more routes; return NULL if MHD couldn't
 * bind the test port (probably already in use — test will skip). */
static api_server_t *start_test_server(api_auth_fn auth_fn)
{
    api_server_t *srv = api_server_create(TEST_PORT, NULL, NULL);
    if (!srv) return NULL;
    if (auth_fn) api_server_set_auth(srv, auth_fn, NULL);
    api_server_route(srv, "/api/public",     API_GET, int_handler_ok,          NULL);
    api_server_route(srv, "/api/protected",  API_GET, int_handler_ok,          NULL);
    api_server_route(srv, "/api/echo",       API_GET, int_handler_echo_query,  NULL);
    api_server_route(srv, "/api/post",       API_POST, int_handler_ok,         NULL);
    return srv;
}

static long curl_get(const char *url, curl_buf_t *body)
{
    CURL *c = curl_easy_init();
    if (!c) return -1;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, body);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 5L);
    long status = -1;
    if (curl_easy_perform(c) == CURLE_OK)
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    return status;
}

static long curl_post(const char *url, const char *body_in)
{
    curl_buf_t resp = {0};
    CURL *c = curl_easy_init();
    if (!c) return -1;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body_in);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 5L);
    long status = -1;
    if (curl_easy_perform(c) == CURLE_OK)
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(c);
    free(resp.data);
    return status;
}

static void test_integration_basic_get(void **state)
{
    (void)state;
    api_server_t *srv = start_test_server(NULL);
    if (!srv) skip();

    curl_buf_t body = {0};
    long status = curl_get(TEST_BASE_URL "/api/public", &body);
    assert_int_equal(status, 200);
    assert_non_null(body.data);
    assert_non_null(strstr(body.data, "hello"));
    free(body.data);

    api_server_stop(srv);
}

static void test_integration_404(void **state)
{
    (void)state;
    api_server_t *srv = start_test_server(NULL);
    if (!srv) skip();

    curl_buf_t body = {0};
    long status = curl_get(TEST_BASE_URL "/api/nope", &body);
    assert_int_equal(status, 404);
    free(body.data);
    api_server_stop(srv);
}

static void test_integration_query_param(void **state)
{
    (void)state;
    api_server_t *srv = start_test_server(NULL);
    if (!srv) skip();

    curl_buf_t body = {0};
    long status = curl_get(
        TEST_BASE_URL "/api/echo?name=alice", &body);
    assert_int_equal(status, 200);
    assert_non_null(strstr(body.data, "alice"));
    free(body.data);
    api_server_stop(srv);
}

static void test_integration_post(void **state)
{
    (void)state;
    api_server_t *srv = start_test_server(NULL);
    if (!srv) skip();

    long status = curl_post(
        TEST_BASE_URL "/api/post", "{\"x\":1}");
    assert_int_equal(status, 200);
    api_server_stop(srv);
}

static void test_integration_auth_blocks_protected(void **state)
{
    (void)state;
    api_server_t *srv = start_test_server(int_auth_deny_protected);
    if (!srv) skip();

    /* /api/public passes; /api/protected gets 401 */
    curl_buf_t b1 = {0};
    long s1 = curl_get(TEST_BASE_URL "/api/public",    &b1);
    curl_buf_t b2 = {0};
    long s2 = curl_get(TEST_BASE_URL "/api/protected", &b2);

    assert_int_equal(s1, 200);
    assert_int_equal(s2, 401);
    free(b1.data); free(b2.data);
    api_server_stop(srv);
}

#define TEST_STATIC_DIR "/tmp/airies_test_routes_auth_static"

static void write_file(const char *path, const char *contents)
{
    FILE *f = fopen(path, "w");
    if (f) { fputs(contents, f); fclose(f); }
}

static void test_integration_static_file_served(void **state)
{
    (void)state;
    /* Build a tiny static tree the server can serve from */
    mkdir(TEST_STATIC_DIR, 0755);
    write_file(TEST_STATIC_DIR "/index.html",
               "<html><body>hello</body></html>");
    write_file(TEST_STATIC_DIR "/app.js", "console.log(1)");
    write_file(TEST_STATIC_DIR "/style.css", "body{}");

    api_server_t *srv = api_server_create(TEST_PORT, NULL, TEST_STATIC_DIR);
    if (!srv) {
        unlink(TEST_STATIC_DIR "/index.html");
        unlink(TEST_STATIC_DIR "/app.js");
        unlink(TEST_STATIC_DIR "/style.css");
        rmdir(TEST_STATIC_DIR);
        skip();
    }

    /* Root → index.html */
    curl_buf_t b1 = {0};
    long s1 = curl_get(TEST_BASE_URL "/", &b1);
    assert_int_equal(s1, 200);
    assert_non_null(strstr(b1.data, "hello"));

    /* JS file */
    curl_buf_t b2 = {0};
    long s2 = curl_get(TEST_BASE_URL "/app.js", &b2);
    assert_int_equal(s2, 200);

    /* CSS file */
    curl_buf_t b3 = {0};
    long s3 = curl_get(TEST_BASE_URL "/style.css", &b3);
    assert_int_equal(s3, 200);

    /* SPA fallback: /unknown-route (no extension) → index.html */
    curl_buf_t b4 = {0};
    long s4 = curl_get(TEST_BASE_URL "/some/spa/route", &b4);
    assert_int_equal(s4, 200);
    assert_non_null(strstr(b4.data, "hello"));

    /* Static file with extension that doesn't exist → 404 (not SPA
     * fallback, because the path has an extension). */
    curl_buf_t b5 = {0};
    long s5 = curl_get(TEST_BASE_URL "/missing.png", &b5);
    assert_int_equal(s5, 404);

    free(b1.data); free(b2.data); free(b3.data);
    free(b4.data); free(b5.data);
    api_server_stop(srv);

    unlink(TEST_STATIC_DIR "/index.html");
    unlink(TEST_STATIC_DIR "/app.js");
    unlink(TEST_STATIC_DIR "/style.css");
    rmdir(TEST_STATIC_DIR);
}

#define TEST_UNIX_SOCK "/tmp/airies_test_routes_auth.sock"

static void test_integration_unix_socket_create(void **state)
{
    (void)state;
    /* api_server_create with a unix socket path exercises the unix
     * daemon spawn + the socket cleanup in api_server_stop. We don't
     * actually make a request through the socket here — we just
     * exercise the lifecycle. */
    unlink(TEST_UNIX_SOCK);
    api_server_t *srv = api_server_create(0, TEST_UNIX_SOCK, NULL);
    if (!srv) skip();
    api_server_stop(srv);
    /* Socket file should be cleaned up */
    assert_int_not_equal(access(TEST_UNIX_SOCK, F_OK), 0);
}

/* --- api_query_param / api_cookie NULL-conn paths ---
 *
 * These are server.c functions that read from MHD's connection state.
 * The NULL-conn path is a defensive guard that's reachable when called
 * outside an MHD request context (e.g. from a test). */

static void test_query_param_null_conn(void **state)
{
    (void)state;
    api_request_t req = {0};
    /* req._conn = NULL → api_query_param must return NULL */
    assert_null(api_query_param(&req, "anything"));
}

static void test_cookie_null_conn(void **state)
{
    (void)state;
    api_request_t req = {0};
    assert_null(api_cookie(&req, "auth"));
}

int main(void)
{
    const struct CMUnitTest tests[] = {
        cmocka_unit_test_setup_teardown(test_setup_first_run_succeeds,         setup, teardown),
        cmocka_unit_test_setup_teardown(test_setup_already_set_rejected,       setup, teardown),
        cmocka_unit_test_setup_teardown(test_setup_short_password_rejected,    setup, teardown),
        cmocka_unit_test_setup_teardown(test_setup_missing_body_rejected,      setup, teardown),
        cmocka_unit_test_setup_teardown(test_setup_invalid_json_rejected,      setup, teardown),
        cmocka_unit_test_setup_teardown(test_login_correct_password_sets_cookie, setup, teardown),
        cmocka_unit_test_setup_teardown(test_login_secure_flag_when_configured,  setup, teardown),
        cmocka_unit_test_setup_teardown(test_login_wrong_password_no_cookie,     setup, teardown),
        cmocka_unit_test_setup_teardown(test_login_missing_password_field,       setup, teardown),
        cmocka_unit_test_setup_teardown(test_login_missing_body_rejected,        setup, teardown),
        cmocka_unit_test_setup_teardown(test_logout_clears_cookie_and_revokes,        setup, teardown),
        cmocka_unit_test_setup_teardown(test_logout_without_token_still_clears_cookie, setup, teardown),
        cmocka_unit_test_setup_teardown(test_check_returns_ok,                   setup, teardown),
        cmocka_unit_test_setup_teardown(test_change_correct_old_password, setup, teardown),
        cmocka_unit_test_setup_teardown(test_change_wrong_old_password,   setup, teardown),
        cmocka_unit_test_setup_teardown(test_change_short_new_password,   setup, teardown),
        cmocka_unit_test_setup_teardown(test_change_missing_fields,       setup, teardown),
        cmocka_unit_test_setup_teardown(test_setup_status_fresh_install,    setup, teardown),
        cmocka_unit_test_setup_teardown(test_setup_status_password_set,     setup, teardown),
        cmocka_unit_test_setup_teardown(test_setup_status_fully_configured, setup, teardown),
        cmocka_unit_test_setup_teardown(test_setup_ports_returns_arrays,    setup, teardown),
        cmocka_unit_test_setup_teardown(test_setup_test_serial_failure,           setup, teardown),
        cmocka_unit_test_setup_teardown(test_setup_test_usb_failure,              setup, teardown),
        cmocka_unit_test_setup_teardown(test_setup_test_default_serial_missing_device, setup, teardown),
        cmocka_unit_test_setup_teardown(test_setup_test_missing_body,             setup, teardown),
        cmocka_unit_test(test_api_ok),
        cmocka_unit_test(test_api_ok_status),
        cmocka_unit_test(test_api_error_includes_message),
        cmocka_unit_test(test_api_error_null_message),
        cmocka_unit_test(test_api_no_content),
        cmocka_unit_test(test_server_create_no_listeners),
        cmocka_unit_test(test_server_route_registration),
        cmocka_unit_test(test_server_set_auth_callback),
        cmocka_unit_test(test_server_stop_null_safe),
        cmocka_unit_test(test_query_param_null_conn),
        cmocka_unit_test(test_cookie_null_conn),
        cmocka_unit_test(test_integration_basic_get),
        cmocka_unit_test(test_integration_404),
        cmocka_unit_test(test_integration_query_param),
        cmocka_unit_test(test_integration_post),
        cmocka_unit_test(test_integration_auth_blocks_protected),
        cmocka_unit_test(test_integration_static_file_served),
        cmocka_unit_test(test_integration_unix_socket_create),
    };
    return cmocka_run_group_tests(tests, NULL, NULL);
}
