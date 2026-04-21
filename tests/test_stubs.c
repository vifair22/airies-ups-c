/* Stub driver symbols so ups.c links without pulling in real drivers.
 * Each test binary that includes ups.c needs these to satisfy the
 * extern references in the ups_drivers[] registry. */

#include "ups/ups_driver.h"

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

const ups_driver_t ups_driver_backups_hid = {
    .name      = "backups_hid_stub",
    .conn_type = UPS_CONN_USB,
    .topology  = UPS_TOPO_STANDBY,
    .caps      = 0,
};
