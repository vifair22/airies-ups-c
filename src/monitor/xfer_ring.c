#include "monitor/xfer_ring.h"
#include "ups/ups_format.h"

#include <string.h>

void xfer_ring_init(xfer_ring_t *ring)
{
    memset(ring, 0, sizeof(*ring));
}

int xfer_ring_push(xfer_ring_t *ring, uint64_t now_ms, uint16_t reason)
{
    if (ring->last_seen_valid && reason == ring->last_seen)
        return 0;
    ring->entries[ring->head].timestamp_ms = now_ms;
    ring->entries[ring->head].reason       = reason;
    ring->head = (ring->head + 1) % XFER_RING_SIZE;
    if (ring->count < XFER_RING_SIZE) ring->count++;
    ring->last_seen       = reason;
    ring->last_seen_valid = 1;
    return 1;
}

uint16_t xfer_ring_recent_cause(const xfer_ring_t *ring,
                                uint64_t now_ms, uint32_t lookback_ms)
{
    uint64_t cutoff   = (now_ms > lookback_ms) ? now_ms - lookback_ms : 0;
    uint16_t best     = UPS_TRANSFER_REASON_UNKNOWN;
    uint64_t best_ts  = 0;
    for (size_t i = 0; i < ring->count; i++) {
        const xfer_event_t *e = &ring->entries[i];
        if (e->timestamp_ms < cutoff) continue;
        if (e->reason == XFER_REASON_ACCEPTABLE_INPUT) continue;
        if (e->timestamp_ms > best_ts) {
            best    = e->reason;
            best_ts = e->timestamp_ms;
        }
    }
    return best;
}
