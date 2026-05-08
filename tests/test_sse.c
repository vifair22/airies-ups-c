#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <setjmp.h>
#include <cmocka.h>

#include "api/sse.h"
#include "api/server.h"

#include <curl/curl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* --- Helpers --- */

static int frame_matches(const char *buf, size_t len,
                         const char *event_type, const char *payload)
{
    char expected[1024];
    int n = snprintf(expected, sizeof(expected),
                     "event: %s\ndata: %s\n\n", event_type, payload);
    if (n < 0 || (size_t)n != len) return 0;
    return memcmp(buf, expected, len) == 0;
}

/* --- Tests: lifecycle --- */

static void test_create_destroy(void **state)
{
    (void)state;
    sse_broadcaster_t *b = sse_broadcaster_create(0);
    assert_non_null(b);
    assert_int_equal(sse_broadcaster_subscriber_count(b), 0);
    sse_broadcaster_destroy(b);
}

static void test_subscribe_unsubscribe(void **state)
{
    (void)state;
    sse_broadcaster_t *b = sse_broadcaster_create(0);
    sse_subscriber_t *s = sse_broadcaster_subscribe(b);
    assert_non_null(s);
    assert_int_equal(sse_broadcaster_subscriber_count(b), 1);
    sse_subscriber_close(s);
    assert_int_equal(sse_broadcaster_subscriber_count(b), 0);
    sse_broadcaster_destroy(b);
}

static void test_subscribe_caps_at_max(void **state)
{
    (void)state;
    sse_broadcaster_t *b = sse_broadcaster_create(0);
    sse_subscriber_t *subs[SSE_MAX_SUBSCRIBERS];
    for (int i = 0; i < SSE_MAX_SUBSCRIBERS; i++) {
        subs[i] = sse_broadcaster_subscribe(b);
        assert_non_null(subs[i]);
    }
    /* The (max+1)th must be rejected. */
    sse_subscriber_t *over = sse_broadcaster_subscribe(b);
    assert_null(over);
    assert_int_equal(sse_broadcaster_subscriber_count(b), SSE_MAX_SUBSCRIBERS);

    /* Closing one should free a slot for a new subscribe. */
    sse_subscriber_close(subs[3]);
    subs[3] = sse_broadcaster_subscribe(b);
    assert_non_null(subs[3]);

    for (int i = 0; i < SSE_MAX_SUBSCRIBERS; i++)
        sse_subscriber_close(subs[i]);
    sse_broadcaster_destroy(b);
}

/* --- Tests: emit + read --- */

static void test_emit_single_subscriber(void **state)
{
    (void)state;
    sse_broadcaster_t *b = sse_broadcaster_create(60000);  /* long enough not to fire */
    sse_subscriber_t *s = sse_broadcaster_subscribe(b);

    sse_broadcaster_emit(b, "monitor", "{\"k\":1}");

    char buf[256];
    ssize_t n = sse_subscriber_read(s, buf, sizeof(buf));
    assert_true(n > 0);
    assert_true(frame_matches(buf, (size_t)n, "monitor", "{\"k\":1}"));

    sse_subscriber_close(s);
    sse_broadcaster_destroy(b);
}

static void test_emit_fanout_multi(void **state)
{
    (void)state;
    sse_broadcaster_t *b = sse_broadcaster_create(60000);
    sse_subscriber_t *a = sse_broadcaster_subscribe(b);
    sse_subscriber_t *c = sse_broadcaster_subscribe(b);

    sse_broadcaster_emit(b, "monitor", "{\"x\":2}");

    char buf_a[256], buf_c[256];
    ssize_t na = sse_subscriber_read(a, buf_a, sizeof(buf_a));
    ssize_t nc = sse_subscriber_read(c, buf_c, sizeof(buf_c));
    assert_true(na > 0);
    assert_true(nc > 0);
    assert_true(frame_matches(buf_a, (size_t)na, "monitor", "{\"x\":2}"));
    assert_true(frame_matches(buf_c, (size_t)nc, "monitor", "{\"x\":2}"));

    sse_subscriber_close(a);
    sse_subscriber_close(c);
    sse_broadcaster_destroy(b);
}

static void test_queue_overflow_drops_oldest(void **state)
{
    (void)state;
    sse_broadcaster_t *b = sse_broadcaster_create(60000);
    sse_subscriber_t *s = sse_broadcaster_subscribe(b);

    /* Push QUEUE_DEPTH + 1 frames. The first one should be dropped. */
    for (int i = 0; i <= SSE_QUEUE_DEPTH; i++) {
        char payload[32];
        snprintf(payload, sizeof(payload), "{\"i\":%d}", i);
        sse_broadcaster_emit(b, "monitor", payload);
    }
    assert_int_equal(sse_subscriber_queue_depth(s), SSE_QUEUE_DEPTH);

    /* First frame readable should be i=1, not i=0. */
    char buf[256];
    ssize_t n = sse_subscriber_read(s, buf, sizeof(buf));
    assert_true(n > 0);
    assert_true(frame_matches(buf, (size_t)n, "monitor", "{\"i\":1}"));

    sse_subscriber_close(s);
    sse_broadcaster_destroy(b);
}

/* --- Tests: cache (push-on-connect) --- */

static void test_cache_primes_new_subscriber(void **state)
{
    (void)state;
    sse_broadcaster_t *b = sse_broadcaster_create(60000);
    sse_broadcaster_emit_cached(b, "state", "{\"v\":1}");

    sse_subscriber_t *s = sse_broadcaster_subscribe(b);
    assert_int_equal(sse_subscriber_queue_depth(s), 1);

    char buf[256];
    ssize_t n = sse_subscriber_read(s, buf, sizeof(buf));
    assert_true(n > 0);
    assert_true(frame_matches(buf, (size_t)n, "state", "{\"v\":1}"));

    sse_subscriber_close(s);
    sse_broadcaster_destroy(b);
}

static void test_cache_replaces_with_latest(void **state)
{
    (void)state;
    sse_broadcaster_t *b = sse_broadcaster_create(60000);
    sse_broadcaster_emit_cached(b, "state", "{\"v\":1}");
    sse_broadcaster_emit_cached(b, "state", "{\"v\":2}");
    sse_broadcaster_emit_cached(b, "state", "{\"v\":3}");

    sse_subscriber_t *s = sse_broadcaster_subscribe(b);
    /* Only the latest cached frame is primed onto a new subscriber. */
    assert_int_equal(sse_subscriber_queue_depth(s), 1);

    char buf[256];
    ssize_t n = sse_subscriber_read(s, buf, sizeof(buf));
    assert_true(frame_matches(buf, (size_t)n, "state", "{\"v\":3}"));

    sse_subscriber_close(s);
    sse_broadcaster_destroy(b);
}

static void test_cache_does_not_block_emit_to_existing(void **state)
{
    (void)state;
    sse_broadcaster_t *b = sse_broadcaster_create(60000);
    sse_subscriber_t *s = sse_broadcaster_subscribe(b);

    /* emit_cached should fan out *and* update the cache. The existing
     * subscriber must receive the frame even though it's also being
     * cached for future subscribers. */
    sse_broadcaster_emit_cached(b, "state", "{\"v\":7}");
    assert_int_equal(sse_subscriber_queue_depth(s), 1);

    char buf[256];
    ssize_t n = sse_subscriber_read(s, buf, sizeof(buf));
    assert_true(frame_matches(buf, (size_t)n, "state", "{\"v\":7}"));

    sse_subscriber_close(s);
    sse_broadcaster_destroy(b);
}

/* --- Tests: monitor event wrapper --- */

static void test_monitor_event_wrapper_emits_json(void **state)
{
    (void)state;
    sse_broadcaster_t *b = sse_broadcaster_create(60000);
    sse_subscriber_t *s = sse_broadcaster_subscribe(b);

    sse_on_monitor_event("warning", "system", "Title", "Msg with \"quotes\"", b);

    char buf[512];
    ssize_t n = sse_subscriber_read(s, buf, sizeof(buf));
    assert_true(n > 0);
    /* Expect the SSE prefix followed by a JSON object containing all four
     * fields. We don't assert exact JSON ordering; we just probe substrings. */
    buf[n < (ssize_t)sizeof(buf) ? n : (ssize_t)sizeof(buf) - 1] = '\0';
    assert_non_null(strstr(buf, "event: monitor\n"));
    assert_non_null(strstr(buf, "\"severity\""));
    assert_non_null(strstr(buf, "warning"));
    assert_non_null(strstr(buf, "system"));
    assert_non_null(strstr(buf, "Title"));
    /* Embedded quotes should be JSON-escaped, not raw. */
    assert_non_null(strstr(buf, "\\\"quotes\\\""));

    sse_subscriber_close(s);
    sse_broadcaster_destroy(b);
}

/* --- Tests: partial reads + heartbeat + shutdown --- */

static void test_partial_read_drains_across_calls(void **state)
{
    (void)state;
    sse_broadcaster_t *b = sse_broadcaster_create(60000);
    sse_subscriber_t *s = sse_broadcaster_subscribe(b);

    sse_broadcaster_emit(b, "monitor", "{\"long\":\"abcdefghij\"}");

    /* Read in 8-byte chunks until the frame drains. */
    char accum[256] = {0};
    size_t total = 0;
    for (;;) {
        char chunk[8];
        ssize_t n = sse_subscriber_read(s, chunk, sizeof(chunk));
        assert_true(n > 0);
        memcpy(accum + total, chunk, (size_t)n);
        total += (size_t)n;
        /* The frame ends with "\n\n"; once we see that we stop. */
        if (total >= 4 && memcmp(accum + total - 2, "\n\n", 2) == 0)
            break;
        if (total >= sizeof(accum)) break;
    }
    assert_true(frame_matches(accum, total, "monitor", "{\"long\":\"abcdefghij\"}"));

    sse_subscriber_close(s);
    sse_broadcaster_destroy(b);
}

static void test_heartbeat_on_idle(void **state)
{
    (void)state;
    /* 50 ms heartbeat — fires fast in tests. */
    sse_broadcaster_t *b = sse_broadcaster_create(50);
    sse_subscriber_t *s = sse_broadcaster_subscribe(b);

    char buf[64];
    ssize_t n = sse_subscriber_read(s, buf, sizeof(buf));
    assert_int_equal(n, (ssize_t)strlen(":heartbeat\n\n"));
    assert_int_equal(memcmp(buf, ":heartbeat\n\n", (size_t)n), 0);

    sse_subscriber_close(s);
    sse_broadcaster_destroy(b);
}

/* --- Shutdown path: blocked reader returns -1 --- */

struct reader_args {
    sse_subscriber_t *s;
    ssize_t           result;
};

static void *reader_thread(void *arg)
{
    struct reader_args *a = arg;
    char buf[64];
    a->result = sse_subscriber_read(a->s, buf, sizeof(buf));
    return NULL;
}

static void test_shutdown_wakes_blocked_reader(void **state)
{
    (void)state;
    /* Long heartbeat so the reader is genuinely blocked when we shutdown. */
    sse_broadcaster_t *b = sse_broadcaster_create(60000);
    sse_subscriber_t *s = sse_broadcaster_subscribe(b);

    struct reader_args args = { .s = s, .result = 0 };
    pthread_t th;
    assert_int_equal(pthread_create(&th, NULL, reader_thread, &args), 0);

    /* Give the reader a moment to enter pthread_cond_timedwait. */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
    nanosleep(&ts, NULL);

    sse_broadcaster_shutdown(b);

    assert_int_equal(pthread_join(th, NULL), 0);
    assert_int_equal(args.result, -1);

    sse_subscriber_close(s);
    sse_broadcaster_destroy(b);
}

static void test_subscribe_after_shutdown_fails(void **state)
{
    (void)state;
    sse_broadcaster_t *b = sse_broadcaster_create(60000);
    sse_broadcaster_shutdown(b);
    assert_null(sse_broadcaster_subscribe(b));
    sse_broadcaster_destroy(b);
}

/* --- Integration tests: real MHD daemon + libcurl ---
 *
 * Same pattern as tests/test_routes_auth.c — bring up an api_server on a
 * loopback port, hit it with libcurl, exercise the full request_handler ->
 * streaming-route -> SSE broadcaster path. Port 48092 stays out of
 * test_routes_auth's 48091 so the suites can run concurrently.
 *
 * libcurl's `CURLOPT_WRITEFUNCTION` returns 0 to abort the transfer once
 * we've captured enough bytes — that's how we drain a finite chunk from
 * an otherwise unbounded stream. */

#define INT_TEST_PORT     48092
#define INT_TEST_BASE_URL "http://127.0.0.1:48092"

typedef struct {
    char  *data;
    size_t len;
    size_t want;       /* abort transfer once len >= want */
} sse_capture_t;

static size_t sse_write_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    size_t bytes = size * nmemb;
    sse_capture_t *cap = ud;
    char *grown = realloc(cap->data, cap->len + bytes + 1);
    if (!grown) return 0;
    cap->data = grown;
    memcpy(cap->data + cap->len, ptr, bytes);
    cap->len += bytes;
    cap->data[cap->len] = '\0';
    if (cap->want > 0 && cap->len >= cap->want) return 0;
    return bytes;
}

/* Capture response headers separately. */
static size_t sse_header_cb(char *ptr, size_t size, size_t nmemb, void *ud)
{
    sse_capture_t *cap = ud;
    size_t bytes = size * nmemb;
    char *grown = realloc(cap->data, cap->len + bytes + 1);
    if (!grown) return 0;
    cap->data = grown;
    memcpy(cap->data + cap->len, ptr, bytes);
    cap->len += bytes;
    cap->data[cap->len] = '\0';
    return bytes;
}

/* Spawn libcurl in a thread so the main thread can drive the broadcaster
 * (emits, capacity setup) while the curl thread is blocked reading the
 * stream. Times out after 3 s — the abort-via-zero-return write callback
 * is the normal exit path, the timeout is a safety net. */

typedef struct {
    char         url[256];
    sse_capture_t cap;
    long          status;
    long          timeout_ms;
    /* If headers != NULL, capture response headers there too. */
    sse_capture_t *headers;
} curl_thread_args_t;

static void *curl_thread(void *arg)
{
    curl_thread_args_t *a = arg;
    CURL *c = curl_easy_init();
    if (!c) return NULL;
    curl_easy_setopt(c, CURLOPT_URL, a->url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sse_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &a->cap);
    if (a->headers) {
        curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, sse_header_cb);
        curl_easy_setopt(c, CURLOPT_HEADERDATA, a->headers);
    }
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, a->timeout_ms);
    /* Allow CURLE_WRITE_ERROR (returned when our write_cb aborts) — that's
     * the normal completion path. We only care about CURLINFO_RESPONSE_CODE. */
    (void)curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &a->status);
    curl_easy_cleanup(c);
    return NULL;
}

static int int_auth_allow_all(const api_request_t *req, const char *url, void *ud)
{
    (void)req; (void)url; (void)ud;
    return 1;
}

static int int_auth_deny_all(const api_request_t *req, const char *url, void *ud)
{
    (void)req; (void)url; (void)ud;
    return 0;
}

/* --- T1: emit-and-receive over the wire --- */

static void test_int_emit_arrives_via_http(void **state)
{
    (void)state;
    /* Long heartbeat so the test sees only the data frames it expects. */
    sse_broadcaster_t *b = sse_broadcaster_create(60000);
    api_server_t *srv = api_server_create(INT_TEST_PORT, NULL, NULL);
    if (!srv) {
        sse_broadcaster_destroy(b);
        skip();
    }
    api_server_set_auth(srv, int_auth_allow_all, NULL);
    api_server_route_streaming(srv, "/api/events/stream", API_GET,
                               sse_handle_stream, b);

    curl_thread_args_t a = {
        .url        = INT_TEST_BASE_URL "/api/events/stream",
        .cap        = { .want = 64 },
        .timeout_ms = 3000,
    };
    pthread_t th;
    assert_int_equal(pthread_create(&th, NULL, curl_thread, &a), 0);

    /* Wait for the connection to land. The streaming handler subscribes
     * synchronously inside sse_handle_stream — once subscriber_count > 0
     * we know the subscriber is wired and emit() will reach it. */
    for (int i = 0; i < 100 && sse_broadcaster_subscriber_count(b) == 0; i++) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    assert_int_equal(sse_broadcaster_subscriber_count(b), 1);

    sse_broadcaster_emit(b, "monitor", "{\"k\":\"v\"}");

    pthread_join(th, NULL);
    assert_int_equal(a.status, 200);
    assert_non_null(strstr(a.cap.data, "event: monitor"));
    assert_non_null(strstr(a.cap.data, "{\"k\":\"v\"}"));

    free(a.cap.data);
    sse_broadcaster_shutdown(b);
    api_server_stop(srv);
    sse_broadcaster_destroy(b);
}

/* --- T2: push-on-connect via emit_cached --- */

static void test_int_cache_arrives_on_connect(void **state)
{
    (void)state;
    sse_broadcaster_t *b = sse_broadcaster_create(60000);
    api_server_t *srv = api_server_create(INT_TEST_PORT, NULL, NULL);
    if (!srv) {
        sse_broadcaster_destroy(b);
        skip();
    }
    api_server_set_auth(srv, int_auth_allow_all, NULL);
    api_server_route_streaming(srv, "/api/events/stream", API_GET,
                               sse_handle_stream, b);

    /* Prime the cache BEFORE any client connects. */
    sse_broadcaster_emit_cached(b, "state", "{\"primed\":true}");

    sse_capture_t headers = {0};
    curl_thread_args_t a = {
        .url        = INT_TEST_BASE_URL "/api/events/stream",
        .cap        = { .want = 64 },
        .timeout_ms = 3000,
        .headers    = &headers,
    };
    pthread_t th;
    assert_int_equal(pthread_create(&th, NULL, curl_thread, &a), 0);
    pthread_join(th, NULL);

    assert_int_equal(a.status, 200);
    assert_non_null(strstr(a.cap.data, "event: state"));
    assert_non_null(strstr(a.cap.data, "{\"primed\":true}"));
    /* SSE Content-Type + standard cache header must be present. */
    assert_non_null(strstr(headers.data, "Content-Type: text/event-stream"));
    assert_non_null(strstr(headers.data, "Cache-Control: no-cache"));

    free(a.cap.data);
    free(headers.data);
    sse_broadcaster_shutdown(b);
    api_server_stop(srv);
    sse_broadcaster_destroy(b);
}

/* --- T3: auth gate rejects without cookie --- */

static void test_int_auth_gate_rejects(void **state)
{
    (void)state;
    sse_broadcaster_t *b = sse_broadcaster_create(60000);
    api_server_t *srv = api_server_create(INT_TEST_PORT, NULL, NULL);
    if (!srv) {
        sse_broadcaster_destroy(b);
        skip();
    }
    api_server_set_auth(srv, int_auth_deny_all, NULL);
    api_server_route_streaming(srv, "/api/events/stream", API_GET,
                               sse_handle_stream, b);

    curl_thread_args_t a = {
        .url        = INT_TEST_BASE_URL "/api/events/stream",
        .cap        = { .want = 0 },  /* let response complete */
        .timeout_ms = 3000,
    };
    pthread_t th;
    assert_int_equal(pthread_create(&th, NULL, curl_thread, &a), 0);
    pthread_join(th, NULL);

    assert_int_equal(a.status, 401);
    /* No subscriber should have been allocated since auth ran first. */
    assert_int_equal(sse_broadcaster_subscriber_count(b), 0);

    free(a.cap.data);
    api_server_stop(srv);
    sse_broadcaster_destroy(b);
}

/* --- T4: subscriber capacity exceeded -> 503 --- */

static void test_int_capacity_exceeded_returns_503(void **state)
{
    (void)state;
    sse_broadcaster_t *b = sse_broadcaster_create(60000);
    api_server_t *srv = api_server_create(INT_TEST_PORT, NULL, NULL);
    if (!srv) {
        sse_broadcaster_destroy(b);
        skip();
    }
    api_server_set_auth(srv, int_auth_allow_all, NULL);
    api_server_route_streaming(srv, "/api/events/stream", API_GET,
                               sse_handle_stream, b);

    /* Fill all subscriber slots so the HTTP request hits the cap. */
    sse_subscriber_t *fillers[SSE_MAX_SUBSCRIBERS];
    for (int i = 0; i < SSE_MAX_SUBSCRIBERS; i++) {
        fillers[i] = sse_broadcaster_subscribe(b);
        assert_non_null(fillers[i]);
    }

    curl_thread_args_t a = {
        .url        = INT_TEST_BASE_URL "/api/events/stream",
        .cap        = { .want = 0 },
        .timeout_ms = 3000,
    };
    pthread_t th;
    assert_int_equal(pthread_create(&th, NULL, curl_thread, &a), 0);
    pthread_join(th, NULL);

    assert_int_equal(a.status, 503);
    assert_non_null(strstr(a.cap.data, "capacity"));

    free(a.cap.data);
    for (int i = 0; i < SSE_MAX_SUBSCRIBERS; i++)
        sse_subscriber_close(fillers[i]);
    api_server_stop(srv);
    sse_broadcaster_destroy(b);
}

/* --- T5: heartbeat over the wire --- */

static void test_int_heartbeat_arrives(void **state)
{
    (void)state;
    /* Short heartbeat so the test doesn't take 30 seconds. */
    sse_broadcaster_t *b = sse_broadcaster_create(80);
    api_server_t *srv = api_server_create(INT_TEST_PORT, NULL, NULL);
    if (!srv) {
        sse_broadcaster_destroy(b);
        skip();
    }
    api_server_set_auth(srv, int_auth_allow_all, NULL);
    api_server_route_streaming(srv, "/api/events/stream", API_GET,
                               sse_handle_stream, b);

    curl_thread_args_t a = {
        .url        = INT_TEST_BASE_URL "/api/events/stream",
        .cap        = { .want = 13 },  /* ":heartbeat\n\n" is 12 bytes */
        .timeout_ms = 3000,
    };
    pthread_t th;
    assert_int_equal(pthread_create(&th, NULL, curl_thread, &a), 0);
    pthread_join(th, NULL);

    assert_int_equal(a.status, 200);
    assert_non_null(strstr(a.cap.data, ":heartbeat"));

    free(a.cap.data);
    sse_broadcaster_shutdown(b);
    api_server_stop(srv);
    sse_broadcaster_destroy(b);
}

/* --- main --- */

int main(void)
{
    /* libcurl global init for the integration tests. */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_create_destroy),
        cmocka_unit_test(test_subscribe_unsubscribe),
        cmocka_unit_test(test_subscribe_caps_at_max),
        cmocka_unit_test(test_emit_single_subscriber),
        cmocka_unit_test(test_emit_fanout_multi),
        cmocka_unit_test(test_queue_overflow_drops_oldest),
        cmocka_unit_test(test_cache_primes_new_subscriber),
        cmocka_unit_test(test_cache_replaces_with_latest),
        cmocka_unit_test(test_cache_does_not_block_emit_to_existing),
        cmocka_unit_test(test_monitor_event_wrapper_emits_json),
        cmocka_unit_test(test_partial_read_drains_across_calls),
        cmocka_unit_test(test_heartbeat_on_idle),
        cmocka_unit_test(test_shutdown_wakes_blocked_reader),
        cmocka_unit_test(test_subscribe_after_shutdown_fails),
        cmocka_unit_test(test_int_emit_arrives_via_http),
        cmocka_unit_test(test_int_cache_arrives_on_connect),
        cmocka_unit_test(test_int_auth_gate_rejects),
        cmocka_unit_test(test_int_capacity_exceeded_returns_503),
        cmocka_unit_test(test_int_heartbeat_arrives),
    };
    int rc = cmocka_run_group_tests(tests, NULL, NULL);
    curl_global_cleanup();
    return rc;
}
