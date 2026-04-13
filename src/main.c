#include "ups.h"
#include "shutdown.h"
#include "config.h"
#include "log.h"

#define PUSHOVER_IMPLEMENTATION
#include "pushover.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

#define POLL_INTERVAL_SEC 2
#define CONFIG_PATH       "config.ini"

static volatile int running = 1;
static pushover_client po_client;
static int po_enabled = 0;

static void sig_handler(int sig)
{
    (void)sig;
    running = 0;
}

static void notify(const char *title, const char *message)
{
    if (po_enabled)
        pushover_alert(&po_client, title, message);
}

static void print_inventory(const ups_inventory_t *inv)
{
    char msg[512];
    snprintf(msg, sizeof(msg), "Model: %s | Serial: %s | FW: %s | %uVA / %uW",
             inv->model, inv->serial, inv->firmware, inv->nominal_va, inv->nominal_watts);
    log_msg("INFO", msg);
}

static void print_status(const ups_data_t *d)
{
    char status_str[256], eff_str[64];
    ups_status_str(d->status, status_str, sizeof(status_str));
    ups_efficiency_str((int16_t)(d->efficiency * 128), eff_str, sizeof(eff_str));

    char msg[1024];
    snprintf(msg, sizeof(msg),
             "Status: %s | Chg: %.0f%% | Rt: %um%us | Bv: %.1fV | "
             "In: %.1fV | Out: %.1fV %.1fHz %.1fA | Load: %.0f%% | "
             "Eff: %s | Xfer: %s",
             status_str, d->charge_pct,
             d->runtime_sec / 60, d->runtime_sec % 60,
             d->battery_voltage,
             d->input_voltage,
             d->output_voltage, d->output_frequency, d->output_current,
             d->load_pct, eff_str,
             ups_transfer_reason_str(d->transfer_reason));
    log_msg("INFO", msg);
}

/* Build a change-detection signature from status fields */
static uint64_t status_signature(const ups_data_t *d)
{
    /* Combine fields that indicate a meaningful state change */
    return ((uint64_t)d->status << 32) |
           ((uint64_t)d->sig_status << 16) |
           ((uint64_t)d->transfer_reason);
}

static void format_notification(const ups_data_t *d, char *buf, size_t len)
{
    char status_str[256], eff_str[64];
    ups_status_str(d->status, status_str, sizeof(status_str));
    ups_efficiency_str((int16_t)(d->efficiency * 128), eff_str, sizeof(eff_str));

    snprintf(buf, len,
             "Status: %s\n"
             "Battery: %.0f%% (%um%us remaining)\n"
             "Battery Voltage: %.1fV\n"
             "Input: %.1fV\n"
             "Output: %.1fV %.1fHz %.1fA\n"
             "Load: %.0f%%\n"
             "Efficiency: %s\n"
             "Transfer Reason: %s",
             status_str, d->charge_pct,
             d->runtime_sec / 60, d->runtime_sec % 60,
             d->battery_voltage,
             d->input_voltage,
             d->output_voltage, d->output_frequency, d->output_current,
             d->load_pct, eff_str,
             ups_transfer_reason_str(d->transfer_reason));
}

static int should_shutdown(const ups_data_t *d)
{
    /* Trigger on: On Battery + Shutdown Imminent (Sig bit 1) */
    return (d->status & UPS_ST_ON_BATTERY) &&
           (d->sig_status & UPS_SIG_SHUTDOWN_IMMINENT);
}

static void cmd_status(modbus_t *ctx)
{
    ups_inventory_t inv;
    ups_data_t data;

    if (ups_read_inventory(ctx, &inv) == 0)
        print_inventory(&inv);
    else
        log_msg("ERROR", "Failed to read inventory");

    if (ups_read_status(ctx, &data) == 0 && ups_read_dynamic(ctx, &data) == 0)
        print_status(&data);
    else
        log_msg("ERROR", "Failed to read UPS data");
}

static void cmd_test_battery(modbus_t *ctx)
{
    if (ups_cmd_battery_test(ctx) == 0)
        log_msg("INFO", "Battery test started");
    else
        log_msg("ERROR", "Failed to start battery test");
}

static void cmd_monitor(modbus_t *ctx, const config_t *cfg, const shutdown_flags_t *flags)
{
    ups_inventory_t inv;
    ups_data_t data;
    uint64_t prev_sig = 0;
    int first = 1;

    /* Read and display inventory once */
    if (ups_read_inventory(ctx, &inv) == 0) {
        print_inventory(&inv);
    } else {
        log_msg("ERROR", "Failed to read inventory");
    }

    /* Initial read + notification */
    if (ups_read_status(ctx, &data) == 0 && ups_read_dynamic(ctx, &data) == 0) {
        print_status(&data);
        char body[2048];
        format_notification(&data, body, sizeof(body));
        notify("UPS Monitor Started", body);
        prev_sig = status_signature(&data);
        first = 0;
    }

    /* Monitor loop */
    while (running) {
        sleep(POLL_INTERVAL_SEC);

        if (ups_read_status(ctx, &data) != 0 || ups_read_dynamic(ctx, &data) != 0) {
            log_msg("ERROR", "Failed to read UPS data");
            continue;
        }

        /* Check for state change */
        uint64_t sig = status_signature(&data);
        if (!first && sig != prev_sig) {
            print_status(&data);

            char body[2048];
            format_notification(&data, body, sizeof(body));
            notify("UPS Status Change", body);

            /* Check for shutdown condition */
            if (should_shutdown(&data)) {
                char msg[512];
                snprintf(msg, sizeof(msg),
                         "SHUTDOWN TRIGGERED — Battery: %.0f%% Runtime: %um%us",
                         data.charge_pct, data.runtime_sec / 60, data.runtime_sec % 60);
                log_msg("ERROR", msg);

                char alert[2048];
                snprintf(alert, sizeof(alert),
                         "Battery Critical!\n"
                         "Charge: %.0f%%\n"
                         "Runtime: %um%us\n"
                         "Initiating shutdown workflow",
                         data.charge_pct, data.runtime_sec / 60, data.runtime_sec % 60);
                notify("UPS Battery Critical", alert);

                shutdown_workflow(ctx, cfg, flags);
                sleep(180);
                break;
            }

            prev_sig = sig;
        }

        if (first) {
            prev_sig = sig;
            first = 0;
        }
    }
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "  --status            One-shot status dump\n"
            "  --test-battery      Start battery self-test\n"
            "  --test-ssh          Test SSH connectivity to shutdown targets\n"
            "  --reboot-now        Interactive shutdown workflow\n"
            "  --no-ssh-shutdown   Skip SSH host shutdown in workflow\n"
            "  --no-ups-shutdown   Skip UPS shutdown command in workflow\n"
            "  --no-pi-shutdown    Skip local Pi shutdown in workflow\n"
            "  --config <path>     Config file (default: config.ini)\n"
            "  (no args)           Monitor mode\n",
            prog);
}

int main(int argc, char *argv[])
{
    const char *config_path = CONFIG_PATH;
    int mode_status = 0;
    int mode_test_battery = 0;
    int mode_test_ssh = 0;
    int mode_reboot_now = 0;
    shutdown_flags_t sflags = { 0, 0, 0 };

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--status") == 0)
            mode_status = 1;
        else if (strcmp(argv[i], "--test-battery") == 0)
            mode_test_battery = 1;
        else if (strcmp(argv[i], "--test-ssh") == 0)
            mode_test_ssh = 1;
        else if (strcmp(argv[i], "--reboot-now") == 0)
            mode_reboot_now = 1;
        else if (strcmp(argv[i], "--no-ssh-shutdown") == 0)
            sflags.skip_ssh = 1;
        else if (strcmp(argv[i], "--no-ups-shutdown") == 0)
            sflags.skip_ups = 1;
        else if (strcmp(argv[i], "--no-pi-shutdown") == 0)
            sflags.skip_pi = 1;
        else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc)
            config_path = argv[++i];
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    /* Load config */
    config_t cfg;
    if (config_load(&cfg, config_path) != 0) {
        fprintf(stderr, "Failed to load config from %s\n", config_path);
        return 1;
    }

    /* SSH test doesn't need UPS or Pushover */
    if (mode_test_ssh) {
        shutdown_test_ssh(&cfg);
        return 0;
    }

    /* Init Pushover */
    if (cfg.pushover_token[0] && cfg.pushover_user[0]) {
        if (pushover_global_init() == 0) {
            pushover_client_init(&po_client);
            pushover_client_set_auth(&po_client, cfg.pushover_token, cfg.pushover_user);
            po_enabled = 1;
            log_msg("INFO", "Pushover notifications enabled");
        } else {
            log_msg("WARN", "Failed to init Pushover (curl), notifications disabled");
        }
    } else {
        log_msg("INFO", "Pushover not configured, notifications disabled");
    }

    /* Connect to UPS */
    log_msg("INFO", "Connecting to UPS...");
    modbus_t *ctx = ups_connect(cfg.ups_device, cfg.ups_baud, cfg.ups_slave_id);
    if (!ctx) {
        log_msg("ERROR", "Failed to connect to UPS");
        return 1;
    }

    char msg[512];
    snprintf(msg, sizeof(msg), "Connected to %s (baud %d, slave %d)",
             cfg.ups_device, cfg.ups_baud, cfg.ups_slave_id);
    log_msg("INFO", msg);

    /* Signal handling */
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    /* Dispatch mode */
    if (mode_status) {
        cmd_status(ctx);
    } else if (mode_test_battery) {
        cmd_test_battery(ctx);
    } else if (mode_reboot_now) {
        printf("Are you sure you want to reboot NOW? [y/N]: ");
        fflush(stdout);
        char answer[16];
        if (fgets(answer, sizeof(answer), stdin)) {
            if (answer[0] == 'y' || answer[0] == 'Y') {
                log_msg("INFO", "User confirmed reboot");
                shutdown_workflow(ctx, &cfg, &sflags);
                sleep(180);
            } else {
                log_msg("INFO", "Reboot aborted by user");
            }
        }
    } else {
        cmd_monitor(ctx, &cfg, &sflags);
    }

    /* Cleanup */
    ups_close(ctx);
    if (po_enabled)
        pushover_global_cleanup();

    log_msg("INFO", "Exiting");
    return 0;
}
