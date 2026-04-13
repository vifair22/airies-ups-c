#ifndef CONFIG_H
#define CONFIG_H

#define CFG_MAX_HOSTS 8
#define CFG_MAX_STR   256

typedef struct {
    /* UPS */
    char ups_device[CFG_MAX_STR];
    int  ups_baud;
    int  ups_slave_id;

    /* Pushover */
    char pushover_token[CFG_MAX_STR];
    char pushover_user[CFG_MAX_STR];

    /* Shutdown targets — UnRaid only */
    char unraid_hosts[CFG_MAX_HOSTS][CFG_MAX_STR];
    int  unraid_host_count;
    char unraid_user[CFG_MAX_STR];
    char unraid_pass[CFG_MAX_STR];
    int  shutdown_timeout;

    /* Alert thresholds */
    int alert_load_high_pct;
    int alert_battery_low_pct;
    int alert_voltage_warn_offset;
    int alert_voltage_deadband;

    /* IPC paths */
    char ipc_sock_path[CFG_MAX_STR];
    char ipc_lock_path[CFG_MAX_STR];
} config_t;

/* Parse config file, returns 0 on success */
int config_load(config_t *cfg, const char *path);

#endif
