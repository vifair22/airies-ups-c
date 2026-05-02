/* Stub driver symbols so ups.c links without pulling in real drivers.
 * Each test binary that includes ups.c needs these to satisfy the
 * extern references in the ups_drivers[] registry.
 *
 * Also stubs out monitor_fire_event so shutdown.c links without pulling
 * in the whole monitor module + its libmodbus/threading deps. The
 * shutdown unit tests never wire a monitor (mgr->monitor is always NULL)
 * so the real implementation is unreachable from these binaries — the
 * stub exists purely to satisfy the linker, since `if (!mgr->monitor)
 * return;` is a runtime check, not a compile-time one. */

#include "ups/ups_driver.h"
#include "monitor/monitor.h"

const ups_driver_t ups_driver_srt = {
    .name      = "srt_stub",
    .conn_type = UPS_CONN_SERIAL,
    .topology  = UPS_TOPO_ONLINE_DOUBLE,
    .caps      = 0,
};

const ups_driver_t ups_driver_smt = {
    .name      = "smt_stub",
    .conn_type = UPS_CONN_SERIAL,
    .topology  = UPS_TOPO_LINE_INTERACTIVE,
    .caps      = 0,
};

const ups_driver_t ups_driver_apc_hid = {
    .name      = "apc_hid_stub",
    .conn_type = UPS_CONN_USB,
    .topology  = UPS_TOPO_STANDBY,
    .caps      = 0,
};

const ups_driver_t ups_driver_cyberpower_hid = {
    .name      = "cyberpower_hid_stub",
    .conn_type = UPS_CONN_USB,
    .topology  = UPS_TOPO_LINE_INTERACTIVE,
    .caps      = 0,
};

void monitor_fire_event(monitor_t *mon, const char *severity,
                        const char *category, const char *title,
                        const char *message)
{
    (void)mon; (void)severity; (void)category; (void)title; (void)message;
}

/* Used by handle_setup_status when ctx->monitor is non-NULL. The
 * route-handler tests construct ctx with monitor=NULL so this stub is
 * never reached at runtime; it just satisfies the linker. */
int monitor_is_connected(monitor_t *mon) { (void)mon; return 0; }
