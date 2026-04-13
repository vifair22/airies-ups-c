#include "shutdown.h"
#include "ups.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>

static int is_host_online(const char *host)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "ping -c 1 -W 1 %s >/dev/null 2>&1", host);
    return system(cmd) == 0;
}

static void wait_for_host_down(const char *host, int timeout_sec)
{
    char msg[512];
    snprintf(msg, sizeof(msg), "[%s] Waiting for host to go offline...", host);
    log_msg("INFO", msg);

    time_t start = time(NULL);
    while (is_host_online(host)) {
        if (time(NULL) - start >= timeout_sec) {
            snprintf(msg, sizeof(msg), "[%s] Timed out after %ds", host, timeout_sec);
            log_msg("WARN", msg);
            return;
        }
        sleep(5);
    }

    snprintf(msg, sizeof(msg), "[%s] Host is down. Waiting 15s grace period...", host);
    log_msg("INFO", msg);
    sleep(15);
}

static void shutdown_unraid_host(const char *host, const char *user, const char *pass, int timeout)
{
    char msg[512];

    if (!is_host_online(host)) {
        snprintf(msg, sizeof(msg), "[%s] Already offline, skipping", host);
        log_msg("WARN", msg);
        return;
    }

    snprintf(msg, sizeof(msg), "[%s] Sending shutdown command...", host);
    log_msg("INFO", msg);

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "sshpass -p '%s' ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 %s@%s powerdown",
             pass, user, host);

    int ret = system(cmd);
    if (ret == 0) {
        snprintf(msg, sizeof(msg), "[%s] Shutdown command sent", host);
        log_msg("INFO", msg);
    } else {
        snprintf(msg, sizeof(msg), "[%s] Shutdown command failed (exit %d)", host, ret);
        log_msg("ERROR", msg);
    }

    wait_for_host_down(host, timeout);
}

static void phase1_ssh_shutdown(const config_t *cfg)
{
    log_msg("INFO", "Phase 1: Shutting down UnRaid hosts...");
    time_t start = time(NULL);

    /* Fork a child for each host */
    pid_t pids[CFG_MAX_HOSTS];
    int count = cfg->unraid_host_count;

    for (int i = 0; i < count; i++) {
        pids[i] = fork();
        if (pids[i] == 0) {
            /* Child process */
            shutdown_unraid_host(cfg->unraid_hosts[i], cfg->unraid_user,
                                 cfg->unraid_pass, cfg->shutdown_timeout);
            _exit(0);
        } else if (pids[i] < 0) {
            char msg[256];
            snprintf(msg, sizeof(msg), "fork() failed for host %s", cfg->unraid_hosts[i]);
            log_msg("ERROR", msg);
        }
    }

    /* Wait for all children */
    for (int i = 0; i < count; i++) {
        if (pids[i] > 0)
            waitpid(pids[i], NULL, 0);
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "Phase 1 complete in %lds", time(NULL) - start);
    log_msg("INFO", msg);
}

static void phase2_ups_shutdown(modbus_t *ctx)
{
    log_msg("INFO", "Phase 2: Sending UPS shutdown command...");

    if (ups_cmd_shutdown(ctx) == 0)
        log_msg("INFO", "UPS shutdown command accepted (~60s countdown)");
    else
        log_msg("ERROR", "UPS shutdown command FAILED");

    sleep(5);
}

void shutdown_workflow(modbus_t *ctx, const config_t *cfg, const shutdown_flags_t *flags)
{
    log_msg("INFO", "=== SHUTDOWN WORKFLOW STARTED ===");
    time_t start = time(NULL);

    /* Phase 1: SSH */
    if (flags->skip_ssh) {
        log_msg("WARN", "Phase 1: SKIPPED (--no-ssh-shutdown)");
    } else {
        phase1_ssh_shutdown(cfg);
    }

    /* Phase 2: UPS */
    if (flags->skip_ups) {
        log_msg("WARN", "Phase 2: SKIPPED (--no-ups-shutdown)");
    } else {
        phase2_ups_shutdown(ctx);
    }

    /* Phase 3: Local Pi shutdown */
    if (flags->skip_pi) {
        log_msg("WARN", "Phase 3: SKIPPED (--no-pi-shutdown)");
    } else {
        log_msg("INFO", "Phase 3: Shutting down this host...");
        system("sudo shutdown -h now");
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "=== SHUTDOWN WORKFLOW COMPLETE in %lds ===", time(NULL) - start);
    log_msg("INFO", msg);
}

void shutdown_test_ssh(const config_t *cfg)
{
    log_msg("INFO", "=== SSH CONNECTIVITY TEST ===");

    for (int i = 0; i < cfg->unraid_host_count; i++) {
        const char *host = cfg->unraid_hosts[i];
        char msg[512];

        if (!is_host_online(host)) {
            snprintf(msg, sizeof(msg), "[%s] Host not reachable (ping failed)", host);
            log_msg("ERROR", msg);
            continue;
        }

        snprintf(msg, sizeof(msg), "[%s] Ping OK, testing SSH...", host);
        log_msg("INFO", msg);

        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
                 "sshpass -p '%s' ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 %s@%s 'echo SSH_OK && uptime'",
                 cfg->unraid_pass, cfg->unraid_user, host);

        int ret = system(cmd);
        if (ret == 0) {
            snprintf(msg, sizeof(msg), "[%s] SSH connection successful", host);
            log_msg("INFO", msg);
        } else {
            snprintf(msg, sizeof(msg), "[%s] SSH connection FAILED (exit %d)", host, ret);
            log_msg("ERROR", msg);
        }
    }

    log_msg("INFO", "=== SSH TEST COMPLETE ===");
}
