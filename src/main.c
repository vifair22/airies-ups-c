#include "ups.h"
#include "shutdown.h"
#include "config.h"
#include "ipc.h"
#include "alerts.h"
#include "log.h"

#define PUSHOVER_IMPLEMENTATION
#include "pushover.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>

#define POLL_INTERVAL_SEC 2
#define CONFIG_PATH       "config.ini"
#define HE_INHIBIT_FILE   "he_inhibit"

/* Operating mode register values */
#define UPS_MODE_HE     64
#define UPS_MODE_ONLINE  2

static volatile int running = 1;
static pushover_client po_client;
static int po_enabled = 0;
static int he_inhibit_active = 0;
static char he_inhibit_source[32] = "";
static int he_reengaged_count = 0;

/* Require 30 consecutive polls (~60s) with HE active before
 * auto-clearing inhibit. Filters transitional HE flicker during
 * the ~5 minute stabilization after a mode change. */
#define HE_REENGAGE_THRESHOLD 30

/* --- HE inhibit state file ---
 * File contains the source of the inhibit: "manual" or "weather" */

static int he_inhibit_read(void)
{
    FILE *f = fopen(HE_INHIBIT_FILE, "r");
    if (!f) {
        he_inhibit_source[0] = '\0';
        return 0;
    }
    if (fgets(he_inhibit_source, sizeof(he_inhibit_source), f)) {
        /* Strip trailing newline */
        char *nl = strchr(he_inhibit_source, '\n');
        if (nl) *nl = '\0';
    } else {
        strncpy(he_inhibit_source, "manual", sizeof(he_inhibit_source) - 1);
    }
    fclose(f);
    return 1;
}

static void he_inhibit_set(const char *source)
{
    FILE *f = fopen(HE_INHIBIT_FILE, "w");
    if (f) {
        fprintf(f, "%s\n", source);
        fclose(f);
    }
    strncpy(he_inhibit_source, source, sizeof(he_inhibit_source) - 1);
    he_inhibit_source[sizeof(he_inhibit_source) - 1] = '\0';
    he_inhibit_active = 1;
}

static void he_inhibit_clear(void)
{
    unlink(HE_INHIBIT_FILE);
    he_inhibit_active = 0;
    he_inhibit_source[0] = '\0';
}

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
    if (he_inhibit_active && !(d->status & UPS_ST_HE_MODE)) {
        char inhibit_tag[64];
        snprintf(inhibit_tag, sizeof(inhibit_tag), " (HE inhibited: %s)",
                 he_inhibit_source[0] ? he_inhibit_source : "unknown");
        strncat(status_str, inhibit_tag, sizeof(status_str) - strlen(status_str) - 1);
    }
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

/* Format status into buffer for IPC responses and notifications */
static void format_status(const ups_data_t *d, char *buf, size_t len)
{
    char status_str[256], eff_str[64];
    ups_status_str(d->status, status_str, sizeof(status_str));
    if (he_inhibit_active && !(d->status & UPS_ST_HE_MODE)) {
        char inhibit_tag[64];
        snprintf(inhibit_tag, sizeof(inhibit_tag), " (HE inhibited: %s)",
                 he_inhibit_source[0] ? he_inhibit_source : "unknown");
        strncat(status_str, inhibit_tag, sizeof(status_str) - strlen(status_str) - 1);
    }
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

/* Build a change-detection signature from status fields */
static uint64_t status_signature(const ups_data_t *d)
{
    return ((uint64_t)d->status << 32) |
           ((uint64_t)d->sig_status << 16) |
           ((uint64_t)d->transfer_reason);
}

static int should_shutdown(const ups_data_t *d)
{
    return (d->status & UPS_ST_ON_BATTERY) &&
           (d->sig_status & UPS_SIG_SHUTDOWN_IMMINENT);
}

/* --- One-shot commands (direct Modbus) --- */

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

static void cmd_clear_faults(modbus_t *ctx)
{
    if (ups_cmd_clear_faults(ctx) == 0)
        log_msg("INFO", "Faults cleared");
    else
        log_msg("ERROR", "Failed to clear faults");
}

static void cmd_mute(modbus_t *ctx)
{
    if (ups_cmd_mute_alarm(ctx) == 0)
        log_msg("INFO", "Alarm muted");
    else
        log_msg("ERROR", "Failed to mute alarm");
}

static void cmd_unmute(modbus_t *ctx)
{
    if (ups_cmd_cancel_mute(ctx) == 0)
        log_msg("INFO", "Alarm unmuted");
    else
        log_msg("ERROR", "Failed to unmute alarm");
}

static void cmd_beep(modbus_t *ctx)
{
    if (ups_cmd_beep_test(ctx) == 0)
        log_msg("INFO", "Beep test sent");
    else
        log_msg("ERROR", "Failed to send beep test");
}

static void cmd_bypass(modbus_t *ctx, int enable)
{
    int rc;
    if (enable)
        rc = ups_cmd_bypass_enable(ctx);
    else
        rc = ups_cmd_bypass_disable(ctx);

    if (rc == 0) {
        log_msg("INFO", enable ? "Bypass enabled" : "Bypass disabled");
        /* Read back status to confirm */
        ups_data_t data;
        if (ups_read_status(ctx, &data) == 0) {
            int in_bypass = (data.status & UPS_ST_BYPASS) != 0;
            char msg[128];
            snprintf(msg, sizeof(msg), "UPS confirms: %s",
                     in_bypass ? "In Bypass" : "Normal operation");
            log_msg("INFO", msg);
        }
    } else {
        log_msg("ERROR", enable ? "Failed to enable bypass"
                                : "Failed to disable bypass");
    }
}

static void cmd_mode(modbus_t *ctx, uint16_t mode, const char *source)
{
    if (ups_cmd_set_mode(ctx, mode) == 0) {
        if (mode == UPS_MODE_HE) {
            he_inhibit_clear();
            log_msg("INFO", "HE mode requested, inhibit cleared — transition takes ~30 seconds");
        } else {
            he_inhibit_set(source);
            char msg[128];
            snprintf(msg, sizeof(msg), "Online mode requested (%s), HE inhibit set — HE will drop within ~5 seconds", source);
            log_msg("INFO", msg);
        }
    } else {
        log_msg("ERROR", "Failed to set operating mode");
    }
}

/* Interactive confirmation — returns 1 if user confirms */
static int confirm(const char *prompt)
{
    printf("%s [y/N]: ", prompt);
    fflush(stdout);
    char answer[16];
    if (fgets(answer, sizeof(answer), stdin))
        return (answer[0] == 'y' || answer[0] == 'Y');
    return 0;
}

/* --- IPC command handler (called from monitor loop) --- */

static void handle_ipc_command(modbus_t *ctx, int client_fd, ipc_cmd_t cmd)
{
    ups_data_t data;
    ups_inventory_t inv;
    int rc;

    switch (cmd) {
    case IPC_CMD_STATUS:
        if (ups_read_inventory(ctx, &inv) == 0) {
            ipc_respond(client_fd, "OK\n");
            ipc_respond(client_fd, "Model: %s | Serial: %s | FW: %s | %uVA / %uW\n",
                        inv.model, inv.serial, inv.firmware,
                        inv.nominal_va, inv.nominal_watts);
        } else {
            ipc_respond(client_fd, "OK\n");
        }
        if (ups_read_status(ctx, &data) == 0 && ups_read_dynamic(ctx, &data) == 0) {
            char buf[2048];
            format_status(&data, buf, sizeof(buf));
            ipc_respond(client_fd, "%s\n", buf);
        } else {
            ipc_respond(client_fd, "ERR Failed to read UPS data\n");
        }
        break;

    case IPC_CMD_BYPASS_ON:
        rc = ups_cmd_bypass_enable(ctx);
        if (rc == 0) {
            ipc_respond(client_fd, "OK\nBypass enabled\n");
            if (ups_read_status(ctx, &data) == 0) {
                ipc_respond(client_fd, "UPS confirms: %s\n",
                            (data.status & UPS_ST_BYPASS) ? "In Bypass" : "Normal operation");
            }
        } else {
            ipc_respond(client_fd, "ERR Failed to enable bypass\n");
        }
        break;

    case IPC_CMD_BYPASS_OFF:
        rc = ups_cmd_bypass_disable(ctx);
        if (rc == 0) {
            ipc_respond(client_fd, "OK\nBypass disabled\n");
            if (ups_read_status(ctx, &data) == 0) {
                ipc_respond(client_fd, "UPS confirms: %s\n",
                            (data.status & UPS_ST_BYPASS) ? "In Bypass" : "Normal operation");
            }
        } else {
            ipc_respond(client_fd, "ERR Failed to disable bypass\n");
        }
        break;

    case IPC_CMD_MODE_HE:
        rc = ups_cmd_set_mode(ctx, UPS_MODE_HE);
        if (rc == 0) {
            he_inhibit_clear();
            ipc_respond(client_fd, "OK\nHE mode requested, inhibit cleared — transition takes ~30 seconds\n");
        } else {
            ipc_respond(client_fd, "ERR Failed to set HE mode\n");
        }
        break;

    case IPC_CMD_MODE_ONLINE:
        rc = ups_cmd_set_mode(ctx, UPS_MODE_ONLINE);
        if (rc == 0) {
            he_inhibit_set("manual");
            ipc_respond(client_fd, "OK\nOnline mode requested (manual), HE inhibit set — HE will drop within ~5 seconds\n");
        } else {
            ipc_respond(client_fd, "ERR Failed to set online mode\n");
        }
        break;

    case IPC_CMD_MODE_ONLINE_WEATHER:
        rc = ups_cmd_set_mode(ctx, UPS_MODE_ONLINE);
        if (rc == 0) {
            he_inhibit_set("weather");
            ipc_respond(client_fd, "OK\nOnline mode requested (weather), HE inhibit set — HE will drop within ~5 seconds\n");
        } else {
            ipc_respond(client_fd, "ERR Failed to set online mode\n");
        }
        break;

    case IPC_CMD_TEST_BATTERY:
        rc = ups_cmd_battery_test(ctx);
        ipc_respond(client_fd, rc == 0
            ? "OK\nBattery test started\n"
            : "ERR Failed to start battery test\n");
        break;

    case IPC_CMD_CLEAR_FAULTS:
        rc = ups_cmd_clear_faults(ctx);
        ipc_respond(client_fd, rc == 0
            ? "OK\nFaults cleared\n"
            : "ERR Failed to clear faults\n");
        break;

    case IPC_CMD_MUTE:
        rc = ups_cmd_mute_alarm(ctx);
        ipc_respond(client_fd, rc == 0
            ? "OK\nAlarm muted\n"
            : "ERR Failed to mute alarm\n");
        break;

    case IPC_CMD_UNMUTE:
        rc = ups_cmd_cancel_mute(ctx);
        ipc_respond(client_fd, rc == 0
            ? "OK\nAlarm unmuted\n"
            : "ERR Failed to unmute alarm\n");
        break;

    case IPC_CMD_BEEP:
        rc = ups_cmd_beep_test(ctx);
        ipc_respond(client_fd, rc == 0
            ? "OK\nBeep test sent\n"
            : "ERR Failed to send beep test\n");
        break;

    case IPC_CMD_UNKNOWN:
        ipc_respond(client_fd, "ERR Unknown command\n");
        break;
    }

    ipc_respond_end(client_fd);
}

/* --- Monitor loop --- */

static void cmd_monitor(modbus_t *ctx, const config_t *cfg,
                        const shutdown_flags_t *flags, int listen_fd)
{
    ups_inventory_t inv;
    ups_data_t data;
    uint64_t prev_sig = 0;
    uint32_t prev_status = 0;
    int first = 1;

    /* Alert system */
    alert_state_t alert_state;
    alerts_init(&alert_state);

    ups_thresholds_t thresholds = { 0, 0 };

    /* Read and display inventory once */
    if (ups_read_inventory(ctx, &inv) == 0)
        print_inventory(&inv);
    else
        log_msg("ERROR", "Failed to read inventory");

    /* Reconcile HE inhibit state from prior run */
    he_inhibit_active = he_inhibit_read();
    if (he_inhibit_active) {
        /* Check if UPS re-engaged HE on its own */
        ups_data_t check;
        if (ups_read_status(ctx, &check) == 0 && (check.status & UPS_ST_HE_MODE)) {
            log_msg("INFO", "HE inhibit file found but UPS is in HE mode — clearing stale inhibit");
            he_inhibit_clear();
        } else {
            log_msg("INFO", "HE inhibit active from prior session");
        }
    }

    /* Read transfer thresholds for alert system */
    if (ups_read_thresholds(ctx, &thresholds.transfer_high, &thresholds.transfer_low) == 0) {
        char msg[128];
        snprintf(msg, sizeof(msg), "Transfer thresholds: high=%uV low=%uV",
                 thresholds.transfer_high, thresholds.transfer_low);
        log_msg("INFO", msg);
    } else {
        log_msg("WARN", "Failed to read transfer thresholds, voltage alerts disabled");
    }

    /* Initial read + notification */
    if (ups_read_status(ctx, &data) == 0 && ups_read_dynamic(ctx, &data) == 0) {
        print_status(&data);
        char body[2048];
        format_status(&data, body, sizeof(body));
        notify("UPS Monitor Started", body);
        prev_sig = status_signature(&data);
        prev_status = data.status;
        first = 0;
    }

    /* Poll loop with IPC */
    struct pollfd pfds[2];
    int client_fd = -1;

    pfds[0].fd = listen_fd;
    pfds[0].events = POLLIN;
    pfds[1].fd = -1;
    pfds[1].events = 0;

    while (running) {
        int nfds = (client_fd >= 0) ? 2 : 1;
        int timeout_ms = POLL_INTERVAL_SEC * 1000;
        poll(pfds, (nfds_t)nfds, timeout_ms);

        /* Accept new connection */
        if (pfds[0].revents & POLLIN) {
            int new_fd = accept(listen_fd, NULL, NULL);
            if (new_fd >= 0) {
                if (client_fd >= 0)
                    close(client_fd);
                client_fd = new_fd;
                pfds[1].fd = client_fd;
                pfds[1].events = POLLIN;
            }
        }

        /* Handle client command */
        if (client_fd >= 0 && (pfds[1].revents & POLLIN)) {
            char cmd_buf[IPC_MAX_CMD_LEN];
            ssize_t n = read(client_fd, cmd_buf, sizeof(cmd_buf) - 1);
            if (n <= 0) {
                close(client_fd);
                client_fd = -1;
                pfds[1].fd = -1;
            } else {
                cmd_buf[n] = '\0';
                ipc_cmd_t cmd = ipc_parse_command(cmd_buf);
                handle_ipc_command(ctx, client_fd, cmd);
                close(client_fd);
                client_fd = -1;
                pfds[1].fd = -1;
            }
        }

        /* Periodic Modbus poll */
        if (ups_read_status(ctx, &data) != 0 || ups_read_dynamic(ctx, &data) != 0) {
            log_msg("ERROR", "Failed to read UPS data");
            continue;
        }

        /* Auto-clear HE inhibit if UPS has stably re-engaged HE.
         * Require consecutive polls to filter transitional flicker. */
        if (he_inhibit_active) {
            if (data.status & UPS_ST_HE_MODE) {
                he_reengaged_count++;
                if (he_reengaged_count >= HE_REENGAGE_THRESHOLD) {
                    log_msg("INFO", "UPS stably re-engaged HE mode, clearing inhibit state");
                    he_inhibit_clear();
                    he_reengaged_count = 0;
                    notify("UPS HE Mode Restored", "HE mode re-engaged by UPS, inhibit override cleared");
                }
            } else {
                if (he_reengaged_count > 0)
                    he_reengaged_count = 0;
            }
        }

        /* Run threshold alert checks */
        uint32_t alerted = alerts_check(&alert_state, &data, &thresholds, cfg, notify);

        /* Check for state change */
        uint64_t sig = status_signature(&data);
        if (!first && sig != prev_sig) {
            print_status(&data);

            /* Determine if generic notification is needed.
             * Suppress if all status bit changes are covered by dedicated alerts
             * AND sig_status/transfer_reason are unchanged. */
            uint32_t status_changed = prev_status ^ data.status;
            uint16_t sig_changed = (uint16_t)((prev_sig >> 16) ^ (sig >> 16));
            uint16_t xfer_changed = (uint16_t)(prev_sig ^ sig);

            int generic_needed = (sig_changed != 0) ||
                                 (xfer_changed != 0) ||
                                 ((status_changed & ~alerted) != 0);

            if (generic_needed) {
                char body[2048];
                format_status(&data, body, sizeof(body));
                notify("UPS Status Change", body);
            }

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
            prev_status = data.status;
        }

        if (first) {
            prev_sig = sig;
            prev_status = data.status;
            first = 0;
        }
    }

    if (client_fd >= 0)
        close(client_fd);
}

/* --- CLI mode enumeration --- */

typedef enum {
    MODE_MONITOR,
    MODE_STATUS,
    MODE_TEST_BATTERY,
    MODE_TEST_SSH,
    MODE_REBOOT_NOW,
    MODE_BYPASS,
    MODE_SET_MODE,
    MODE_CLEAR_FAULTS,
    MODE_MUTE,
    MODE_UNMUTE,
    MODE_BEEP,
} cli_mode_t;

/* Map CLI modes to IPC command strings */
static const char *mode_to_ipc_cmd(cli_mode_t mode, int bypass_on, uint16_t set_mode_val)
{
    switch (mode) {
    case MODE_STATUS:       return "STATUS";
    case MODE_TEST_BATTERY: return "TEST BATTERY";
    case MODE_BYPASS:       return bypass_on ? "BYPASS ON" : "BYPASS OFF";
    case MODE_SET_MODE:     return set_mode_val == UPS_MODE_HE ? "MODE HE" : "MODE ONLINE";
    case MODE_CLEAR_FAULTS: return "CLEAR FAULTS";
    case MODE_MUTE:         return "MUTE";
    case MODE_UNMUTE:       return "UNMUTE";
    case MODE_BEEP:         return "BEEP";
    default:                return NULL;
    }
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [options]\n"
            "\n"
            "Commands:\n"
            "  --status            One-shot status dump\n"
            "  --test-battery      Start battery self-test\n"
            "  --test-ssh          Test SSH connectivity to shutdown targets\n"
            "  --reboot-now        Interactive shutdown workflow\n"
            "  --bypass on|off     Enable/disable bypass mode (requires confirmation)\n"
            "  --mode he|online    Set operating mode (HE or Online)\n"
            "  --clear-faults      Clear UPS fault conditions\n"
            "  --mute              Mute active alarms\n"
            "  --unmute            Cancel alarm mute\n"
            "  --beep              Short LED/beeper test\n"
            "\n"
            "Options:\n"
            "  --no-ssh-shutdown   Skip SSH host shutdown in workflow\n"
            "  --no-ups-shutdown   Skip UPS shutdown command in workflow\n"
            "  --no-pi-shutdown    Skip local Pi shutdown in workflow\n"
            "  --config <path>     Config file (default: config.ini)\n"
            "  (no args)           Monitor mode\n",
            prog);
}

/* Send a one-shot command via IPC to the running monitor */
static int oneshot_via_ipc(const config_t *cfg, const char *ipc_cmd)
{
    int sock = ipc_client_connect(cfg->ipc_sock_path);
    if (sock < 0) return -1;

    if (ipc_client_send(sock, ipc_cmd) != 0) {
        close(sock);
        return -1;
    }

    char response[IPC_MAX_RESPONSE];
    if (ipc_client_recv(sock, response, sizeof(response)) == 0) {
        printf("%s", response);
        close(sock);
        /* Check for ERR prefix on first line */
        return (strncmp(response, "ERR", 3) == 0) ? 1 : 0;
    }

    close(sock);
    fprintf(stderr, "Error reading response from monitor\n");
    return 1;
}

int main(int argc, char *argv[])
{
    const char *config_path = CONFIG_PATH;
    cli_mode_t mode = MODE_MONITOR;
    int bypass_on = 0;
    uint16_t set_mode_val = 0;
    shutdown_flags_t sflags = { 0, 0, 0 };

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--status") == 0) {
            mode = MODE_STATUS;
        } else if (strcmp(argv[i], "--test-battery") == 0) {
            mode = MODE_TEST_BATTERY;
        } else if (strcmp(argv[i], "--test-ssh") == 0) {
            mode = MODE_TEST_SSH;
        } else if (strcmp(argv[i], "--reboot-now") == 0) {
            mode = MODE_REBOOT_NOW;
        } else if (strcmp(argv[i], "--bypass") == 0 && i + 1 < argc) {
            mode = MODE_BYPASS;
            i++;
            if (strcmp(argv[i], "on") == 0)
                bypass_on = 1;
            else if (strcmp(argv[i], "off") == 0)
                bypass_on = 0;
            else {
                fprintf(stderr, "Invalid bypass argument: %s (expected on|off)\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = MODE_SET_MODE;
            i++;
            if (strcmp(argv[i], "he") == 0)
                set_mode_val = UPS_MODE_HE;
            else if (strcmp(argv[i], "online") == 0)
                set_mode_val = UPS_MODE_ONLINE;
            else {
                fprintf(stderr, "Invalid mode argument: %s (expected he|online)\n", argv[i]);
                return 1;
            }
        } else if (strcmp(argv[i], "--clear-faults") == 0) {
            mode = MODE_CLEAR_FAULTS;
        } else if (strcmp(argv[i], "--mute") == 0) {
            mode = MODE_MUTE;
        } else if (strcmp(argv[i], "--unmute") == 0) {
            mode = MODE_UNMUTE;
        } else if (strcmp(argv[i], "--beep") == 0) {
            mode = MODE_BEEP;
        } else if (strcmp(argv[i], "--no-ssh-shutdown") == 0) {
            sflags.skip_ssh = 1;
        } else if (strcmp(argv[i], "--no-ups-shutdown") == 0) {
            sflags.skip_ups = 1;
        } else if (strcmp(argv[i], "--no-pi-shutdown") == 0) {
            sflags.skip_pi = 1;
        } else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
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

    /* SSH test doesn't need UPS, Pushover, or IPC */
    if (mode == MODE_TEST_SSH) {
        shutdown_test_ssh(&cfg);
        return 0;
    }

    /* Bypass requires interactive confirmation before anything else */
    if (mode == MODE_BYPASS) {
        const char *prompt = bypass_on
            ? "Enable bypass mode? This removes battery protection."
            : "Disable bypass mode? This returns to normal operation.";
        if (!confirm(prompt)) {
            log_msg("INFO", "Bypass operation cancelled by user");
            return 0;
        }
    }

    /* For one-shot commands, try IPC to running monitor first */
    if (mode != MODE_MONITOR && mode != MODE_TEST_SSH && mode != MODE_REBOOT_NOW) {
        const char *ipc_cmd = mode_to_ipc_cmd(mode, bypass_on, set_mode_val);
        if (ipc_cmd) {
            int rc = oneshot_via_ipc(&cfg, ipc_cmd);
            if (rc >= 0) return rc;
            /* IPC failed — fall through to direct Modbus */
            log_msg("INFO", "Monitor not running, connecting directly to UPS");
        }
    }

    /* Init Pushover (needed for monitor and some one-shot feedback) */
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

    int exit_code = 0;

    /* Dispatch */
    switch (mode) {
    case MODE_STATUS:
        cmd_status(ctx);
        break;
    case MODE_TEST_BATTERY:
        cmd_test_battery(ctx);
        break;
    case MODE_BYPASS:
        cmd_bypass(ctx, bypass_on);
        break;
    case MODE_SET_MODE:
        cmd_mode(ctx, set_mode_val, "manual");
        break;
    case MODE_CLEAR_FAULTS:
        cmd_clear_faults(ctx);
        break;
    case MODE_MUTE:
        cmd_mute(ctx);
        break;
    case MODE_UNMUTE:
        cmd_unmute(ctx);
        break;
    case MODE_BEEP:
        cmd_beep(ctx);
        break;
    case MODE_REBOOT_NOW:
        if (confirm("Are you sure you want to reboot NOW?")) {
            log_msg("INFO", "User confirmed reboot");
            shutdown_workflow(ctx, &cfg, &sflags);
            sleep(180);
        } else {
            log_msg("INFO", "Reboot aborted by user");
        }
        break;
    case MODE_MONITOR: {
        /* Acquire PID lock */
        int lock_fd = ipc_lock_acquire(cfg.ipc_lock_path);
        if (lock_fd < 0) {
            log_msg("ERROR", "Another monitor instance is already running");
            exit_code = 1;
            break;
        }

        /* Create IPC socket */
        int listen_fd = ipc_server_init(cfg.ipc_sock_path);
        if (listen_fd < 0) {
            log_msg("ERROR", "Failed to create IPC socket");
            ipc_lock_release(lock_fd, cfg.ipc_lock_path);
            exit_code = 1;
            break;
        }

        log_msg("INFO", "IPC socket listening");
        cmd_monitor(ctx, &cfg, &sflags, listen_fd);

        ipc_server_cleanup(cfg.ipc_sock_path, listen_fd);
        ipc_lock_release(lock_fd, cfg.ipc_lock_path);
        break;
    }
    default:
        break;
    }

    /* Cleanup */
    ups_close(ctx);
    if (po_enabled)
        pushover_global_cleanup();

    log_msg("INFO", "Exiting");
    return exit_code;
}
