#ifndef SHUTDOWN_H
#define SHUTDOWN_H

#include "config.h"
#include <modbus/modbus.h>

typedef struct {
    int skip_ssh;
    int skip_ups;
    int skip_pi;
} shutdown_flags_t;

/* Run the full shutdown workflow.
 * Phase 1: SSH shutdown UnRaid hosts (parallel)
 * Phase 2: UPS shutdown command
 * Phase 3: Local Pi shutdown
 */
void shutdown_workflow(modbus_t *ctx, const config_t *cfg, const shutdown_flags_t *flags);

/* Test SSH connectivity to all configured hosts without shutting down */
void shutdown_test_ssh(const config_t *cfg);

#endif
