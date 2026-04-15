#include "app_config.h"

#include <cutils/log.h>
#include <cutils/appguard.h>

#include "ups/ups.h"

#include <stdio.h>

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    /* Initialize c-utils (config, DB, migrations, logging, push) */
    appguard_config_t ag_cfg = app_appguard_config();
    appguard_t *guard = appguard_init(&ag_cfg);
    if (!guard)
        return 1;

    cutils_config_t *cfg = appguard_config(guard);
    log_info("airies-upsd starting");
    log_info("UPS device: %s (baud %d, slave %d)",
             config_get_str(cfg, "ups.device"),
             config_get_int(cfg, "ups.baud", 9600),
             config_get_int(cfg, "ups.slave_id", 1));

    /* TODO: HTTP server, monitor loop, etc. */

    appguard_shutdown(guard);
    return 0;
}
