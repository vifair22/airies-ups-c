#ifndef API_SSE_H
#define API_SSE_H

#include "api/server.h"

#include <stddef.h>
#include <sys/types.h>

/* --- Server-Sent Events broadcaster ---
 *
 * Multi-subscriber fan-out for live UPS events to web-UI clients.
 *
 * Event channels (multiplexed onto one /api/events/stream endpoint via the
 * SSE `event:` field):
 *   - `monitor` : state-change events (alerts, transitions). Fired by the
 *                 monitor subsystem; broadcaster's monitor wrapper formats
 *                 the payload.
 *   - `state`   : periodic snapshot of the full ups_data view. Cached and
 *                 primed onto new subscribers so reconnecting clients see
 *                 the latest snapshot without waiting for the next tick.
 *
 * Concurrency model:
 *   - emit_*  is called from the event-firing thread (slow loop / fast loop)
 *   - subscribe + subscriber_read run in MHD worker threads (one per conn)
 *   - subscriber_close runs from the MHD content-reader cleanup callback
 *
 * Lock order: broadcaster -> subscriber. Walking the subscriber list and
 * pushing into per-subscriber queues is done while holding the broadcaster
 * mutex, so subscribers cannot disappear mid-emit.
 *
 * Subscribers do not auto-free. Owners (the streaming-route plumbing in
 * server.c) call sse_subscriber_close once the reader has returned -1.
 * sse_broadcaster_shutdown wakes all blocked readers so they return -1
 * during daemon teardown. */

#define SSE_MAX_SUBSCRIBERS       8
#define SSE_QUEUE_DEPTH           256
#define SSE_DEFAULT_HEARTBEAT_MS  30000

typedef struct sse_broadcaster sse_broadcaster_t;
typedef struct sse_subscriber  sse_subscriber_t;

/* --- Lifecycle --- */

/* Create a broadcaster. heartbeat_ms = 0 means SSE_DEFAULT_HEARTBEAT_MS.
 * Returns NULL on alloc / pthread init failure. */
sse_broadcaster_t *sse_broadcaster_create(int heartbeat_ms);

/* Free the broadcaster. All subscribers must already have been closed
 * (via sse_subscriber_close after their reader returned -1). */
void sse_broadcaster_destroy(sse_broadcaster_t *b);

/* Mark the broadcaster as shutting down and wake every blocked
 * subscriber reader so they return -1. Idempotent. After this the
 * streaming-route plumbing should drain (MHD calls cleanup -> close).
 * Then sse_broadcaster_destroy is safe. */
void sse_broadcaster_shutdown(sse_broadcaster_t *b);

/* --- Emit (firing side) --- */

/* Format an SSE frame ("event: <type>\ndata: <payload>\n\n") and fan
 * out to every connected subscriber. Payload should be single-line
 * JSON — newlines inside data: lines would break frame delimiting. */
void sse_broadcaster_emit(sse_broadcaster_t *b,
                          const char *event_type,
                          const char *json_payload);

/* Same as emit, but also stores the formatted frame as the broadcaster's
 * cached state. New subscribers receive this frame as their first
 * message. Used for the `state` channel. */
void sse_broadcaster_emit_cached(sse_broadcaster_t *b,
                                 const char *event_type,
                                 const char *json_payload);

/* monitor_event_fn-shaped wrapper. userdata = sse_broadcaster_t*.
 * Builds {"severity","category","title","message"} JSON and calls
 * sse_broadcaster_emit(b, "monitor", ...). */
void sse_on_monitor_event(const char *severity, const char *category,
                          const char *title, const char *message,
                          void *userdata);

/* --- Subscriber (consumer side, MHD worker thread) --- */

/* Register a new subscriber. Returns NULL if the broadcaster is full
 * or shutting down. If a cached state frame is present, it is enqueued
 * as the subscriber's first message (push-on-connect). */
sse_subscriber_t *sse_broadcaster_subscribe(sse_broadcaster_t *b);

/* Unsubscribe and free. Must be called exactly once per successful
 * subscribe. The caller must guarantee no concurrent subscriber_read
 * is in flight on this subscriber — typically called from the MHD
 * content-reader cleanup, which runs after the reader returns. */
void sse_subscriber_close(sse_subscriber_t *s);

/* Read up to `max` bytes from the subscriber's queue into `buf`,
 * blocking up to the broadcaster's heartbeat interval.
 *   > 0 : bytes copied (data or heartbeat).
 *   == 0: buf too small to hold the next message / heartbeat. Caller
 *         should retry with a larger buf (>= SSE_HEARTBEAT_LEN).
 *   == -1: subscriber closed (broadcaster shutdown or remote disconnect).
 */
ssize_t sse_subscriber_read(sse_subscriber_t *s, char *buf, size_t max);

/* --- Streaming-route handler ---
 *
 * Plug this into api_server_route_streaming. Subscribes to the broadcaster
 * (passed via userdata), wraps the subscriber as the stream's reader, and
 * cleans up via subscriber_close on disconnect. Falls back to a 503 if
 * the broadcaster is full or shutting down. */
api_stream_response_t sse_handle_stream(const api_request_t *req, void *userdata);

/* --- Test introspection --- */

size_t sse_broadcaster_subscriber_count(sse_broadcaster_t *b);
size_t sse_subscriber_queue_depth(sse_subscriber_t *s);

#endif /* API_SSE_H */
