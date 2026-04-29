#ifndef MONITOR_XFER_RING_H
#define MONITOR_XFER_RING_H

#include <stddef.h>
#include <stdint.h>

/* --- Transfer-reason ring buffer ---
 *
 * The fast-poll thread reads register 2 of the SRT/SMT status block every
 * ~200 ms and pushes value transitions into a ring. The slow status poll
 * (2 s) reads back from the ring when annotating events so brief mains
 * glitches that resolve before the next slow poll still surface their
 * cause — APC firmware writes "AcceptableInput" back to register 2 once
 * mains is good again, so the slow read alone misses the actual reason.
 *
 * The data structure is intentionally pure / no I/O: monitor.c owns the
 * mutex and the fast-poll thread; this module just stores transitions
 * and answers "what's the most recent non-AcceptableInput value?". Keeps
 * the lookup logic unit-testable without dragging in pthread / Modbus. */

#define XFER_RING_SIZE 16

/* Register-2 enum value for "no recent input event" — the steady-state
 * value the SRT/SMT firmware writes back once input is acceptable. The
 * cause-lookup ignores this value so "Acceptable Input" never gets
 * surfaced as the cause of a non-input event (e.g. an HE-mode toggle
 * driven by load or temperature). */
#define XFER_REASON_ACCEPTABLE_INPUT  8u

typedef struct {
    uint64_t timestamp_ms;
    uint16_t reason;
} xfer_event_t;

typedef struct {
    xfer_event_t entries[XFER_RING_SIZE];
    size_t       head;             /* next write index */
    size_t       count;            /* valid entries (capped at XFER_RING_SIZE) */
    uint16_t     last_seen;        /* last value read (for change detect) */
    int          last_seen_valid;  /* 0 until first push() */
} xfer_ring_t;

/* Reset the ring to empty. */
void xfer_ring_init(xfer_ring_t *ring);

/* Record a fresh reading. Returns 1 if the value differs from the most
 * recent entry (the change was recorded); 0 if it duplicates the last
 * seen value (no entry added — only transitions are interesting). */
int xfer_ring_push(xfer_ring_t *ring, uint64_t now_ms, uint16_t reason);

/* Returns the most recent non-AcceptableInput reason recorded within
 * `lookback_ms` of `now_ms`. UPS_TRANSFER_REASON_UNKNOWN when nothing
 * relevant lands in the window — callers should drop any "(reason: ...)"
 * suffix in that case so events don't get annotated with stale values. */
uint16_t xfer_ring_recent_cause(const xfer_ring_t *ring,
                                uint64_t now_ms, uint32_t lookback_ms);

#endif
