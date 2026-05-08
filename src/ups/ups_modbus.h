#ifndef UPS_MODBUS_H
#define UPS_MODBUS_H

#include <modbus/modbus.h>
#include <stdint.h>

/* --- Paced Modbus transport ---
 *
 * Thin wrapper over libmodbus that enforces a minimum gap between
 * consecutive operations on the same transport. The SMT management
 * plane crashes if polled too fast (≥50 ms between requests is the
 * empirically-derived floor — see project memory). SRT tolerates
 * faster pacing but shares the wrapper because there's no downside.
 *
 * Pacing is measured end-of-previous-op to start-of-next-op, so
 * latency-heavy ops don't accumulate drift. Timestamps use
 * CLOCK_MONOTONIC so wall-clock slew (NTP, DST) doesn't send the
 * gap calculation backwards.
 *
 * Thread safety: callers serialize access via ups->cmd_mutex (held
 * by all ups_read_* / ups_*_write callers in ups.c). The wrapper
 * adds no additional locking.
 *
 * HID drivers do not use this module — they go through hidapi
 * directly. */

#define UPS_MB_MIN_GAP_MS 50

typedef struct {
    modbus_t *mb;
    uint64_t  last_op_ms;
} ups_mb_t;

/* Allocate, configure, and connect a paced Modbus RTU transport.
 * Returns NULL on any failure (modbus_new_rtu, modbus_connect).
 * Frees the underlying modbus_t on failure paths. */
ups_mb_t *ups_mb_new(const char *device, int baud, int slave_id);

/* Close and free the transport. Safe to pass NULL. */
void ups_mb_free(ups_mb_t *m);

/* Paced reads/writes. Return values mirror libmodbus: number of
 * registers read/written on success, -1 on error. */
int ups_mb_read   (ups_mb_t *m, int addr, int n, uint16_t *regs);
int ups_mb_write  (ups_mb_t *m, int addr, uint16_t value);
int ups_mb_write_n(ups_mb_t *m, int addr, int n, const uint16_t *regs);

#endif /* UPS_MODBUS_H */
