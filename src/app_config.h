#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <cutils/config.h>
#include <cutils/appguard.h>

/* App config key definitions and AppGuard configuration.
 *
 * File-backed keys are in the YAML config file (needed before DB starts).
 * DB-backed keys are stored in SQLite (mutable at runtime via API). */

/* --- File-backed config keys --- */

static const config_key_t app_file_keys[] = {
    /* UPS connection */
    { "ups.device",    CFG_STRING, "/dev/ttyUSB0", "Serial port for UPS Modbus RTU",
      CFG_STORE_FILE, 1 },
    { "ups.baud",      CFG_INT,    "9600",         "Modbus baud rate",
      CFG_STORE_FILE, 0 },
    { "ups.slave_id",  CFG_INT,    "1",            "Modbus slave ID",
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
    { "monitor.poll_interval",      CFG_INT, "2",
      "UPS status poll interval (seconds)",
      CFG_STORE_DB, 0 },
    { "monitor.telemetry_interval", CFG_INT, "30",
      "Telemetry DB write interval (seconds)",
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
    { NULL, NULL }  /* sentinel */
};

/* --- AppGuard configuration builder --- */

static inline appguard_config_t app_appguard_config(void)
{
    appguard_config_t cfg = {
        .app_name           = "airies-ups",
        .config_path        = NULL,  /* auto-detect: config.yaml next to binary */
        .on_first_run       = CFG_FIRST_RUN_CONTINUE,
        .file_keys          = app_file_keys,
        .db_keys            = app_db_keys,
        .sections           = app_sections,
        .enable_pushover    = 1,
        .log_retention_days = 30,
        .log_level          = LOG_INFO,
        .migrations_dir     = "migrations",
    };
    return cfg;
}

#endif
