#include "ups/ups_modbus.h"

#include <stdlib.h>
#include <time.h>

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* Sleep until at least UPS_MB_MIN_GAP_MS has elapsed since the last
 * completed op. No-op on the first call (last_op_ms == 0). */
static void pace(ups_mb_t *m)
{
    if (m->last_op_ms == 0) return;
    uint64_t now = monotonic_ms();
    uint64_t elapsed = now - m->last_op_ms;
    if (elapsed >= UPS_MB_MIN_GAP_MS) return;

    uint64_t sleep_ms = UPS_MB_MIN_GAP_MS - elapsed;
    struct timespec ts = {
        .tv_sec  = (time_t)(sleep_ms / 1000),
        .tv_nsec = (long)((sleep_ms % 1000) * 1000000L),
    };
    nanosleep(&ts, NULL);
}

ups_mb_t *ups_mb_new(const char *device, int baud, int slave_id)
{
    modbus_t *ctx = modbus_new_rtu(device, baud, 'N', 8, 1);
    if (!ctx) return NULL;

    modbus_set_slave(ctx, slave_id);
    modbus_set_response_timeout(ctx, 5, 0);

    if (modbus_connect(ctx) == -1) {
        modbus_free(ctx);
        return NULL;
    }

    ups_mb_t *m = calloc(1, sizeof(*m));
    if (!m) {
        modbus_close(ctx);
        modbus_free(ctx);
        return NULL;
    }
    m->mb = ctx;
    return m;
}

void ups_mb_free(ups_mb_t *m)
{
    if (!m) return;
    if (m->mb) {
        modbus_close(m->mb);
        modbus_free(m->mb);
    }
    free(m);
}

int ups_mb_read(ups_mb_t *m, int addr, int n, uint16_t *regs)
{
    pace(m);
    int rc = modbus_read_registers(m->mb, addr, n, regs);
    m->last_op_ms = monotonic_ms();
    return rc;
}

int ups_mb_write(ups_mb_t *m, int addr, uint16_t value)
{
    pace(m);
    int rc = modbus_write_register(m->mb, addr, value);
    m->last_op_ms = monotonic_ms();
    return rc;
}

int ups_mb_write_n(ups_mb_t *m, int addr, int n, const uint16_t *regs)
{
    pace(m);
    int rc = modbus_write_registers(m->mb, addr, n, regs);
    m->last_op_ms = monotonic_ms();
    return rc;
}
