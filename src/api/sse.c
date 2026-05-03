#include "api/sse.h"

#include <cutils/error.h>
#include <cutils/json.h>
#include <cutils/log.h>
#include <cutils/mem.h>

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HEARTBEAT_FRAME     ":heartbeat\n\n"
#define HEARTBEAT_FRAME_LEN (sizeof(HEARTBEAT_FRAME) - 1)

/* A queued SSE frame for one subscriber.
 *   data: malloc'd frame bytes (no terminator required, len is authoritative).
 *   pos:  bytes already returned by sse_subscriber_read for this frame.
 *         Frame stays at the queue head until pos == len, then it's freed
 *         and the head advances. Lets MHD readers with small buffers drain
 *         a frame across multiple read calls. */
struct sse_frame {
    char  *data;
    size_t len;
    size_t pos;
};

struct sse_subscriber {
    sse_broadcaster_t *broadcaster;
    int                slot;            /* index into broadcaster->subs[] */
    pthread_mutex_t    mutex;
    pthread_cond_t     cond;
    struct sse_frame   ring[SSE_QUEUE_DEPTH];
    size_t             head;            /* index of oldest frame */
    size_t             count;           /* number of queued frames */
    int                closed;
};

struct sse_broadcaster {
    pthread_mutex_t    mutex;
    sse_subscriber_t  *subs[SSE_MAX_SUBSCRIBERS];
    size_t             n_subs;
    char              *cached_frame;
    size_t             cached_frame_len;
    int                heartbeat_ms;
    int                shutting_down;
};

/* --- Frame formatting --- */

/* Format "event: <type>\ndata: <payload>\n\n" into a freshly malloc'd
 * buffer. *out_len receives the byte length (no terminator counted).
 * Returns 0 on success, -1 on alloc / format failure. */
static int format_frame(const char *event_type, const char *payload,
                        char **out, size_t *out_len)
{
    if (!event_type || !payload || !out || !out_len) return -1;
    /* "event: " + type + "\ndata: " + payload + "\n\n" + NUL */
    size_t need = 7 + strlen(event_type) + 7 + strlen(payload) + 2 + 1;
    char *buf = malloc(need);
    if (!buf) return -1;
    int n = snprintf(buf, need, "event: %s\ndata: %s\n\n",
                     event_type, payload);
    if (n < 0 || (size_t)n >= need) {
        free(buf);
        return -1;
    }
    *out = buf;
    *out_len = (size_t)n;
    return 0;
}

/* --- Subscriber queue ops (called with subscriber mutex held) --- */

static void subscriber_drop_oldest(sse_subscriber_t *s)
{
    if (s->count == 0) return;
    free(s->ring[s->head].data);
    s->ring[s->head].data = NULL;
    s->ring[s->head].len  = 0;
    s->ring[s->head].pos  = 0;
    s->head = (s->head + 1) % SSE_QUEUE_DEPTH;
    s->count--;
}

/* Take ownership of `frame` (will free on overflow / close). frame_len is
 * the byte length, not including any terminator. */
static void subscriber_enqueue_locked(sse_subscriber_t *s,
                                      char *frame, size_t frame_len)
{
    if (s->closed) {
        free(frame);
        return;
    }
    if (s->count == SSE_QUEUE_DEPTH) {
        /* Drop oldest to make room for newest. SSE has no replay; if the
         * client is slow enough to fill 256 frames behind, dropping a
         * stale one is the right thing. */
        subscriber_drop_oldest(s);
    }
    size_t tail = (s->head + s->count) % SSE_QUEUE_DEPTH;
    s->ring[tail].data = frame;
    s->ring[tail].len  = frame_len;
    s->ring[tail].pos  = 0;
    s->count++;
    pthread_cond_signal(&s->cond);
}

/* Push a frame to a subscriber. Takes a reference-by-copy: the frame is
 * duplicated internally so the same source frame can be fanned out to
 * multiple subscribers without ownership games. */
static int subscriber_push_copy(sse_subscriber_t *s,
                                const char *frame, size_t frame_len)
{
    char *dup = malloc(frame_len);
    if (!dup) return -1;
    memcpy(dup, frame, frame_len);

    pthread_mutex_lock(&s->mutex);
    subscriber_enqueue_locked(s, dup, frame_len);
    pthread_mutex_unlock(&s->mutex);
    return 0;
}

/* --- Broadcaster lifecycle --- */

sse_broadcaster_t *sse_broadcaster_create(int heartbeat_ms)
{
    sse_broadcaster_t *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    if (pthread_mutex_init(&b->mutex, NULL) != 0) {
        free(b);
        return NULL;
    }
    b->heartbeat_ms = heartbeat_ms > 0 ? heartbeat_ms : SSE_DEFAULT_HEARTBEAT_MS;
    return b;
}

void sse_broadcaster_destroy(sse_broadcaster_t *b)
{
    if (!b) return;
    /* All subscribers must be closed before destroy. */
    if (b->n_subs > 0) {
        log_warn("sse_broadcaster_destroy: %zu subscribers still attached",
                 b->n_subs);
    }
    free(b->cached_frame);
    pthread_mutex_destroy(&b->mutex);
    free(b);
}

void sse_broadcaster_shutdown(sse_broadcaster_t *b)
{
    if (!b) return;
    pthread_mutex_lock(&b->mutex);
    b->shutting_down = 1;
    for (size_t i = 0; i < SSE_MAX_SUBSCRIBERS; i++) {
        sse_subscriber_t *s = b->subs[i];
        if (!s) continue;
        pthread_mutex_lock(&s->mutex);
        s->closed = 1;
        pthread_cond_signal(&s->cond);
        pthread_mutex_unlock(&s->mutex);
    }
    pthread_mutex_unlock(&b->mutex);
}

/* --- Emit --- */

static void broadcast_frame(sse_broadcaster_t *b,
                            const char *frame, size_t frame_len)
{
    pthread_mutex_lock(&b->mutex);
    for (size_t i = 0; i < SSE_MAX_SUBSCRIBERS; i++) {
        sse_subscriber_t *s = b->subs[i];
        if (!s) continue;
        /* Failure to push to one subscriber must not break fan-out to others;
         * the affected subscriber will simply miss this frame (and be one of
         * 8 slots — operator UI, not safety-critical). */
        (void)subscriber_push_copy(s, frame, frame_len);
    }
    pthread_mutex_unlock(&b->mutex);
}

void sse_broadcaster_emit(sse_broadcaster_t *b,
                          const char *event_type,
                          const char *json_payload)
{
    if (!b) return;
    char *frame = NULL;
    size_t frame_len = 0;
    if (format_frame(event_type, json_payload, &frame, &frame_len) != 0)
        return;
    broadcast_frame(b, frame, frame_len);
    free(frame);
}

void sse_broadcaster_emit_cached(sse_broadcaster_t *b,
                                 const char *event_type,
                                 const char *json_payload)
{
    if (!b) return;
    char *frame = NULL;
    size_t frame_len = 0;
    if (format_frame(event_type, json_payload, &frame, &frame_len) != 0)
        return;
    broadcast_frame(b, frame, frame_len);

    /* Replace cached frame under broadcaster mutex. */
    pthread_mutex_lock(&b->mutex);
    free(b->cached_frame);
    b->cached_frame = frame;          /* take ownership */
    b->cached_frame_len = frame_len;
    pthread_mutex_unlock(&b->mutex);
}

/* monitor_event_fn wrapper. Builds a flat JSON object with the four
 * monitor-event fields and emits on the `monitor` channel. */
void sse_on_monitor_event(const char *severity, const char *category,
                          const char *title, const char *message,
                          void *userdata)
{
    sse_broadcaster_t *b = userdata;
    if (!b) return;

    CUTILS_AUTO_JSON_RESP cutils_json_resp_t *resp = NULL;
    if (json_resp_new(&resp) != CUTILS_OK) return;
    if (json_resp_add_str(resp, "severity", severity ? severity : "") != CUTILS_OK) return;
    if (json_resp_add_str(resp, "category", category ? category : "") != CUTILS_OK) return;
    if (json_resp_add_str(resp, "title",    title    ? title    : "") != CUTILS_OK) return;
    if (json_resp_add_str(resp, "message",  message  ? message  : "") != CUTILS_OK) return;

    CUTILS_AUTOFREE char *payload = NULL;
    size_t payload_len;
    if (json_resp_finalize(resp, &payload, &payload_len) != CUTILS_OK) return;

    sse_broadcaster_emit(b, "monitor", payload);
}

/* --- Subscriber lifecycle --- */

sse_subscriber_t *sse_broadcaster_subscribe(sse_broadcaster_t *b)
{
    if (!b) return NULL;
    sse_subscriber_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    if (pthread_mutex_init(&s->mutex, NULL) != 0) {
        free(s);
        return NULL;
    }

    /* Use CLOCK_MONOTONIC for cond timed-waits so heartbeat scheduling
     * is immune to wall-clock jumps (NTP step, manual time set). */
    pthread_condattr_t cattr;
    if (pthread_condattr_init(&cattr) != 0) {
        pthread_mutex_destroy(&s->mutex);
        free(s);
        return NULL;
    }
    pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
    int cond_rc = pthread_cond_init(&s->cond, &cattr);
    pthread_condattr_destroy(&cattr);
    if (cond_rc != 0) {
        pthread_mutex_destroy(&s->mutex);
        free(s);
        return NULL;
    }
    s->broadcaster = b;
    s->slot = -1;

    pthread_mutex_lock(&b->mutex);
    if (b->shutting_down || b->n_subs >= SSE_MAX_SUBSCRIBERS) {
        pthread_mutex_unlock(&b->mutex);
        pthread_cond_destroy(&s->cond);
        pthread_mutex_destroy(&s->mutex);
        free(s);
        return NULL;
    }
    for (size_t i = 0; i < SSE_MAX_SUBSCRIBERS; i++) {
        if (b->subs[i] == NULL) {
            b->subs[i] = s;
            s->slot = (int)i;
            break;
        }
    }
    b->n_subs++;

    /* Push-on-connect: prime with the cached state frame (if any) so the
     * client doesn't see a blank UI until the next slow-loop tick. */
    if (b->cached_frame && b->cached_frame_len > 0) {
        char *dup = malloc(b->cached_frame_len);
        if (dup) {
            memcpy(dup, b->cached_frame, b->cached_frame_len);
            pthread_mutex_lock(&s->mutex);
            subscriber_enqueue_locked(s, dup, b->cached_frame_len);
            pthread_mutex_unlock(&s->mutex);
        }
    }
    pthread_mutex_unlock(&b->mutex);
    return s;
}

void sse_subscriber_close(sse_subscriber_t *s)
{
    if (!s) return;
    sse_broadcaster_t *b = s->broadcaster;

    pthread_mutex_lock(&b->mutex);
    if (s->slot >= 0 && b->subs[s->slot] == s) {
        b->subs[s->slot] = NULL;
        b->n_subs--;
    }
    pthread_mutex_unlock(&b->mutex);

    /* Drain any queued frames. */
    pthread_mutex_lock(&s->mutex);
    while (s->count > 0) subscriber_drop_oldest(s);
    s->closed = 1;
    pthread_mutex_unlock(&s->mutex);

    pthread_cond_destroy(&s->cond);
    pthread_mutex_destroy(&s->mutex);
    free(s);
}

/* --- Subscriber read --- */

ssize_t sse_subscriber_read(sse_subscriber_t *s, char *buf, size_t max)
{
    if (!s || !buf) return -1;

    /* The heartbeat path needs at least HEARTBEAT_FRAME_LEN bytes; the
     * data path can split a frame across calls so it's fine with smaller
     * buffers. Tell the caller to retry with a bigger buffer rather than
     * silently truncating. */
    if (max == 0) return 0;

    int heartbeat_ms = s->broadcaster->heartbeat_ms;

    pthread_mutex_lock(&s->mutex);
    while (s->count == 0) {
        if (s->closed) {
            pthread_mutex_unlock(&s->mutex);
            return -1;
        }

        struct timespec deadline;
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec  += heartbeat_ms / 1000;
        deadline.tv_nsec += (heartbeat_ms % 1000) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec  += 1;
            deadline.tv_nsec -= 1000000000L;
        }

        int rc = pthread_cond_timedwait(&s->cond, &s->mutex, &deadline);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&s->mutex);
            if (max < HEARTBEAT_FRAME_LEN) return 0;
            memcpy(buf, HEARTBEAT_FRAME, HEARTBEAT_FRAME_LEN);
            return (ssize_t)HEARTBEAT_FRAME_LEN;
        }
        /* Spurious wakeup or signal -> loop and re-check. */
    }

    /* Drain from the head frame; advance head when fully consumed. */
    struct sse_frame *f = &s->ring[s->head];
    size_t avail = f->len - f->pos;
    size_t n = avail < max ? avail : max;
    memcpy(buf, f->data + f->pos, n);
    f->pos += n;
    if (f->pos == f->len) {
        free(f->data);
        f->data = NULL;
        f->len  = 0;
        f->pos  = 0;
        s->head = (s->head + 1) % SSE_QUEUE_DEPTH;
        s->count--;
    }
    pthread_mutex_unlock(&s->mutex);
    return (ssize_t)n;
}

/* --- Streaming-route handler --- */

/* Static header set returned with every successful SSE response.
 *   Cache-Control: no-cache       — clients / intermediaries shouldn't reuse
 *   X-Accel-Buffering: no         — disables nginx response buffering so each
 *                                   frame flushes to the client immediately */
static const api_header_t sse_response_headers[] = {
    { "Content-Type",      "text/event-stream" },
    { "Cache-Control",     "no-cache" },
    { "X-Accel-Buffering", "no" },
    { NULL, NULL },
};

static ssize_t sse_stream_reader(void *cls, char *buf, size_t max)
{
    return sse_subscriber_read((sse_subscriber_t *)cls, buf, max);
}

static void sse_stream_cleanup(void *cls)
{
    sse_subscriber_close((sse_subscriber_t *)cls);
}

api_stream_response_t sse_handle_stream(const api_request_t *req, void *userdata)
{
    (void)req;
    sse_broadcaster_t *b = userdata;

    sse_subscriber_t *s = sse_broadcaster_subscribe(b);
    if (!s) {
        return (api_stream_response_t){
            .status     = 503,
            .error_body = "{\"error\":\"sse capacity exceeded\"}",
        };
    }

    return (api_stream_response_t){
        .status  = 200,
        .reader  = sse_stream_reader,
        .cleanup = sse_stream_cleanup,
        .cls     = s,
        .headers = sse_response_headers,
    };
}

/* --- Test introspection --- */

size_t sse_broadcaster_subscriber_count(sse_broadcaster_t *b)
{
    if (!b) return 0;
    pthread_mutex_lock(&b->mutex);
    size_t n = b->n_subs;
    pthread_mutex_unlock(&b->mutex);
    return n;
}

size_t sse_subscriber_queue_depth(sse_subscriber_t *s)
{
    if (!s) return 0;
    pthread_mutex_lock(&s->mutex);
    size_t n = s->count;
    pthread_mutex_unlock(&s->mutex);
    return n;
}
