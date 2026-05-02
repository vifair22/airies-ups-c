#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <cutils/config.h>
#include <cutils/db.h>
#include <cutils/appguard.h>

/* App config key definitions and AppGuard configuration.
 *
 * File-backed keys are in the YAML config file (needed before DB starts).
 * DB-backed keys are stored in SQLite (mutable at runtime via API). */

/* --- File-backed config keys --- */

static const config_key_t app_file_keys[] = {
    /* UPS connection */
    { "ups.conn_type", CFG_STRING, "serial",       "Connection type: serial or usb",
      CFG_STORE_FILE, 0 },
    { "ups.device",    CFG_STRING, "/dev/ttyUSB0", "Serial port for UPS Modbus RTU",
      CFG_STORE_FILE, 0 },
    { "ups.baud",      CFG_INT,    "9600",         "Modbus baud rate",
      CFG_STORE_FILE, 0 },
    { "ups.slave_id",  CFG_INT,    "1",            "Modbus slave ID",
      CFG_STORE_FILE, 0 },
    { "ups.usb_vid",   CFG_STRING, "051d",         "USB vendor ID (hex)",
      CFG_STORE_FILE, 0 },
    { "ups.usb_pid",   CFG_STRING, "0002",         "USB product ID (hex)",
      CFG_STORE_FILE, 0 },

    /* HTTP server */
    { "http.port",     CFG_INT,    "8080",         "Web UI and API listen port",
      CFG_STORE_FILE, 0 },
    { "http.socket",   CFG_STRING, "/tmp/airies-ups.sock",
      "Unix socket path for CLI communication",
      CFG_STORE_FILE, 0 },

    /* Pushover (c-utils optional, placed in file for persistence) */
    { "pushover.token", CFG_STRING, "", "Pushover API token",
      CFG_STORE_FILE, 0 },
    { "pushover.user",  CFG_STRING, "", "Pushover user key",
      CFG_STORE_FILE, 0 },

    { NULL, 0, NULL, NULL, 0, 0 }  /* sentinel */
};

/* --- DB-backed config keys --- */

static const config_key_t app_db_keys[] = {
    /* Monitor */
    { "monitor.poll_interval",      CFG_INT, "5",
      "UPS status poll interval (seconds) — slow loop only; the fast "
      "power-vitals loop in src/monitor/fast_loop.c runs every 200 ms "
      "regardless and owns power-state events",
      CFG_STORE_DB, 0 },

    /* Alert thresholds */
    { "alerts.load_high_pct",       CFG_INT, "80",
      "Load percentage alert threshold",
      CFG_STORE_DB, 0 },
    { "alerts.battery_low_pct",     CFG_INT, "50",
      "Battery charge low alert threshold",
      CFG_STORE_DB, 0 },
    { "alerts.voltage_warn_offset", CFG_INT, "5",
      "Voltage warning offset from transfer point (V)",
      CFG_STORE_DB, 0 },
    { "alerts.voltage_deadband",    CFG_INT, "1",
      "Voltage alert hysteresis deadband (V)",
      CFG_STORE_DB, 0 },

    /* Pushover notification threshold */
    { "push.min_severity",          CFG_STRING, "warning",
      "Minimum event severity for Pushover notifications (off, info, warning, error, critical)",
      CFG_STORE_DB, 0 },

    /* Bypass voltage window (not readable via Modbus — set to match LCD) */
    { "bypass.voltage_high",        CFG_INT, "140",
      "Bypass upper voltage limit (V) — match UPS LCD setting",
      CFG_STORE_DB, 0 },
    { "bypass.voltage_low",         CFG_INT, "90",
      "Bypass lower voltage limit (V) — match UPS LCD setting",
      CFG_STORE_DB, 0 },

    /* Setup wizard completion flags */
    { "setup.ups_done",             CFG_INT, "0",
      "Set to 1 when UPS connection is configured via setup wizard",
      CFG_STORE_DB, 0 },
    { "setup.ups_name",             CFG_STRING, "",
      "User-defined name for this UPS instance",
      CFG_STORE_DB, 0 },

    /* Shutdown: trigger */
    { "shutdown.trigger",           CFG_STRING, "software",
      "Shutdown trigger mode: software, ups, or manual",
      CFG_STORE_DB, 0 },
    { "shutdown.trigger_source",    CFG_STRING, "runtime",
      "Software trigger source: runtime or field",
      CFG_STORE_DB, 0 },
    { "shutdown.trigger_runtime_sec", CFG_INT, "300",
      "Trigger when runtime drops below this (seconds, 0=disabled)",
      CFG_STORE_DB, 0 },
    { "shutdown.trigger_battery_pct", CFG_INT, "0",
      "Trigger when battery drops below this (%, 0=disabled)",
      CFG_STORE_DB, 0 },
    { "shutdown.trigger_on_battery", CFG_INT, "1",
      "Require on-battery status before triggering (1=yes, 0=no)",
      CFG_STORE_DB, 0 },
    { "shutdown.trigger_delay_sec", CFG_INT, "30",
      "Condition must hold for this many seconds before triggering (debounce)",
      CFG_STORE_DB, 0 },
    { "shutdown.trigger_field",     CFG_STRING, "",
      "UPS data field to watch (e.g. load_pct, input_voltage)",
      CFG_STORE_DB, 0 },
    { "shutdown.trigger_field_op",  CFG_STRING, "lt",
      "Comparison operator: lt, gt, eq",
      CFG_STORE_DB, 0 },
    { "shutdown.trigger_field_value", CFG_INT, "0",
      "Threshold value for the field trigger",
      CFG_STORE_DB, 0 },

    /* Shutdown: UPS action */
    { "shutdown.ups_mode",          CFG_STRING, "command",
      "UPS action on shutdown: command, register, or none",
      CFG_STORE_DB, 0 },
    { "shutdown.ups_register",      CFG_STRING, "",
      "Config register name for register mode",
      CFG_STORE_DB, 0 },
    { "shutdown.ups_value",         CFG_INT, "0",
      "Raw value to write in register mode",
      CFG_STORE_DB, 0 },
    { "shutdown.ups_delay",         CFG_INT, "5",
      "Seconds to wait after UPS action",
      CFG_STORE_DB, 0 },

    /* Shutdown: controller */
    { "shutdown.controller_enabled", CFG_INT, "1",
      "Shut down this controller after all other steps (1=yes, 0=no)",
      CFG_STORE_DB, 0 },

    { NULL, 0, NULL, NULL, 0, 0 }  /* sentinel */
};

/* --- Section display names --- */

static const config_section_t app_sections[] = {
    { "ups",      "UPS Connection" },
    { "http",     "HTTP Server" },
    { "pushover", "Pushover Notifications" },
    { "monitor",  "Monitor Settings" },
    { "alerts",   "Alert Thresholds" },
    { "push",     "Push Notifications" },
    { "bypass",   "Bypass Settings" },
    { "shutdown", "Shutdown Orchestration" },
    { "setup",    "Setup Wizard" },
    { NULL, NULL }  /* sentinel */
};

/* --- Compiled-in migrations (generated by embed_sql.sh) --- */

extern const db_migration_t app_migrations[];

/* --- AppGuard configuration builder --- */

static inline appguard_config_t app_appguard_config(void)
{
    appguard_config_t cfg = {
        .app_name               = "airies-ups",
        /* NULL → c-utils resolves $AIRIES_UPS_CONFIG_PATH first, then
         * falls back to "config.yaml" in CWD. The systemd unit and
         * Dockerfile both set the env var; bare-metal dev runs land on
         * the CWD fallback. */
        .config_path            = NULL,
        .on_first_run           = CFG_FIRST_RUN_CONTINUE,
        .file_keys              = app_file_keys,
        .db_keys                = app_db_keys,
        .sections               = app_sections,
        .enable_pushover        = 1,
        .log_retention_days     = 30,
        .log_level              = LOG_INFO,
        /* When systemd starts us with StandardOutput=journal, c-utils
         * detects JOURNAL_STREAM and switches the console writer to
         * journald-native format: no timestamp (journald stamps every
         * line), no ANSI color, RFC 5424 <N> priority prefix so
         * `journalctl -p err` filters correctly. No-op outside systemd. */
        .log_systemd_autodetect = 1,
        .migrations             = app_migrations,
    };
    return cfg;
}

#endif
