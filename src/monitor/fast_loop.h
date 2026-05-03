#ifndef MONITOR_FAST_LOOP_H
#define MONITOR_FAST_LOOP_H

#include "ups/ups.h"
#include "monitor/monitor.h"
#include <cutils/db.h>
#include <stdint.h>

/* --- Fast power-vitals loop ---
 *
 * Reads the UPS status register and transfer-reason register every
 * ~200 ms (configurable) and owns event emission for the subset of
 * UPS_ST_* bits where sub-2s detection matters:
 *
 *   - UPS_ST_ON_BATTERY
 *   - UPS_ST_OUTPUT_OFF
 *   - UPS_ST_BYPASS
 *   - UPS_ST_FAULT / UPS_ST_FAULT_STATE
 *   - UPS_ST_OVERLOAD
 *
 * Everything else (HE mode, self-test, error registers, voltage
 * thresholds, snapshot) stays on the slow monitor loop.
 *
 * Why dual-loop: the slow poll's interval is too coarse to record
 * brief mains glitches that resolve within one tick. The fast loop
 * sees those, fires events in real time, and persists every
 * register-2 transition to xfer_history so the post-mortem record
 * survives daemon restart.
 *
 * Driver requirements: needs read_transfer_reason (cheap single-reg
 * read of register 2). HID drivers don't expose this and the fast
 * loop is silently skipped for them — those drivers stay slow-loop
 * only, which is acceptable since HID firmware doesn't latch a
 * transfer cause register anyway.
 *
 * Lifecycle: owned by the monitor. monitor_start spawns the fast
 * loop after the slow thread; monitor_stop joins it before the slow
 * thread so it can't fire events while the rest of the monitor is
 * tearing down. */

typedef struct fast_loop fast_loop_t;

/* Spawn the fast loop. Returns NULL on alloc failure or when the
 * driver doesn't expose read_transfer_reason. The returned handle
 * must be released with fast_loop_stop. */
fast_loop_t *fast_loop_start(ups_t *ups, cutils_db_t *db, monitor_t *mon);

/* Signal the loop to exit, join its thread, free resources. Safe
 * to pass NULL. */
void fast_loop_stop(fast_loop_t *fl);

/* Most recent non-AcceptableInput register-2 transition observed
 * within `lookback_ms` of now. Returns UPS_TRANSFER_REASON_UNKNOWN
 * when the window has nothing relevant. Used by the slow loop to
 * annotate HE/test/error events with the cause that triggered them
 * even when the cause has already cleared. */
uint16_t fast_loop_recent_cause(fast_loop_t *fl, uint32_t lookback_ms);

#endif /* MONITOR_FAST_LOOP_H */
