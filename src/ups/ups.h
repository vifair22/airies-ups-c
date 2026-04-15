#ifndef UPS_H
#define UPS_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <modbus/modbus.h>
#include "ups_driver.h"

/* UPSStatus_BF (reg 0-1, uint32) — shared across APC Modbus models (990-9840) */
#define UPS_ST_ONLINE          (1 << 1)
#define UPS_ST_ON_BATTERY      (1 << 2)
#define UPS_ST_BYPASS          (1 << 3)
#define UPS_ST_OUTPUT_OFF      (1 << 4)
#define UPS_ST_FAULT           (1 << 5)
#define UPS_ST_INPUT_BAD       (1 << 6)
#define UPS_ST_TEST            (1 << 7)
#define UPS_ST_PENDING_ON      (1 << 8)
#define UPS_ST_SHUT_PENDING    (1 << 9)
#define UPS_ST_COMMANDED       (1 << 10)  /* SRT: user-initiated bypass */
#define UPS_ST_HE_MODE         (1 << 13)
#define UPS_ST_INFO_ALERT      (1 << 14)  /* SRT: informational alert */
#define UPS_ST_FAULT_STATE     (1 << 15)
#define UPS_ST_MAINS_BAD       (1 << 19)  /* SRT only */
#define UPS_ST_FAULT_RECOVERY  (1 << 20)  /* SRT only */
#define UPS_ST_OVERLOAD        (1 << 21)

/* SimpleSignalingStatus_BF (reg 18) */
#define UPS_SIG_ACTIVE            (1 << 0)
#define UPS_SIG_SHUTDOWN_IMMINENT (1 << 1)

/* GeneralError_BF (reg 19) */
#define UPS_GENERR_SITE_WIRING   (1 << 0)
#define UPS_GENERR_EEPROM        (1 << 1)
#define UPS_GENERR_AD_CONV       (1 << 2)
#define UPS_GENERR_LOGIC_PSU     (1 << 3)
#define UPS_GENERR_INTERNAL_COMM (1 << 4)
#define UPS_GENERR_UI_BUTTON     (1 << 5)
#define UPS_GENERR_EPO_ACTIVE    (1 << 7)
#define UPS_GENERR_FW_MISMATCH   (1 << 8)

/* PowerSystemError_BF (reg 20-21, uint32) */
#define UPS_PWRERR_OVERLOAD      (1 << 0)
#define UPS_PWRERR_SHORT_CIRCUIT (1 << 1)
#define UPS_PWRERR_OVERVOLTAGE   (1 << 2)
#define UPS_PWRERR_OVERTEMP      (1 << 4)
#define UPS_PWRERR_FAN           (1 << 10)
#define UPS_PWRERR_INVERTER      (1 << 13)

/* BatterySystemError_BF (reg 22) */
#define UPS_BATERR_DISCONNECTED  (1 << 0)
#define UPS_BATERR_OVERVOLTAGE   (1 << 1)
#define UPS_BATERR_REPLACE       (1 << 2)
#define UPS_BATERR_OVERTEMP_CRIT (1 << 3)
#define UPS_BATERR_CHARGER       (1 << 4)
#define UPS_BATERR_TEMP_SENSOR   (1 << 5)
#define UPS_BATERR_OVERTEMP_WARN (1 << 7)
#define UPS_BATERR_GENERAL       (1 << 8)
#define UPS_BATERR_COMM          (1 << 9)

/* Error codes */
#define UPS_OK               0
#define UPS_ERR_IO          -1
#define UPS_ERR_NO_DRIVER   -2
#define UPS_ERR_NOT_SUPPORTED -3

/* Parsed UPS status */
struct ups_data {
    /* Status block (reg 0-26) */
    uint32_t status;              /* reg 0-1:  UPSStatus_BF */
    uint16_t transfer_reason;     /* reg 2:    UPSStatusChangeCause_EN */
    uint32_t outlet_mog;          /* reg 3-4:  MOG.OutletStatus_BF */
    uint32_t outlet_sog0;         /* reg 6-7:  SOG[0].OutletStatus_BF */
    uint32_t outlet_sog1;         /* reg 9-10: SOG[1].OutletStatus_BF */
    uint16_t sig_status;          /* reg 18:   SimpleSignalingStatus_BF */
    uint16_t general_error;       /* reg 19:   GeneralError_BF */
    uint32_t power_system_error;  /* reg 20-21: PowerSystemError_BF */
    uint16_t bat_system_error;    /* reg 22:   BatterySystemError_BF */
    uint16_t bat_test_status;     /* reg 23:   ReplaceBatteryTestStatus_BF */
    uint16_t rt_cal_status;       /* reg 24:   RunTimeCalibrationStatus_BF */
    uint16_t bat_lifetime_status; /* reg 25:   Battery.LifeTimeStatus_BF */
    uint16_t ui_status;           /* reg 26:   UserInterfaceStatus_BF */

    /* Dynamic block (reg 128-171) */
    uint32_t runtime_sec;         /* reg 128-129: RunTimeRemaining */
    double   charge_pct;          /* reg 130: StateOfCharge_Pct (/512) */
    double   battery_voltage;     /* reg 131: Battery.Positive.VoltageDC (/32) */
    double   load_pct;            /* reg 136: Output[0].RealPower_Pct (/256) */
    double   output_current;      /* reg 140: Output[0].CurrentAC (/32) */
    double   output_voltage;      /* reg 142: Output[0].VoltageAC (/64) */
    double   output_frequency;    /* reg 144: Output.Frequency (/128) */
    uint32_t output_energy_wh;    /* reg 145-146: Output.Energy */
    uint16_t bypass_status;       /* reg 147: Bypass.InputStatus_BF (SRT only) */
    double   bypass_voltage;      /* reg 148: Bypass.VoltageAC (/64, SRT only) */
    double   bypass_frequency;    /* reg 149: Bypass.Frequency (/128, SRT only) */
    uint16_t input_status;        /* reg 150: Input.InputStatus_BF */
    double   input_voltage;       /* reg 151: Input[0].VoltageAC (/64) */
    double   efficiency;          /* reg 154: Efficiency_EN (/128) */
    int16_t  timer_shutdown;      /* reg 155: MOG.TurnOffCountdown_EN */
    int16_t  timer_start;         /* reg 156: MOG.TurnOnCountdown_EN */
    int32_t  timer_reboot;        /* reg 157-158: MOG.StayOffCountdown_EN */
};

/* Inventory (read once) */
struct ups_inventory {
    char     model[33];
    char     serial[17];
    char     firmware[17];
    uint16_t nominal_va;
    uint16_t nominal_watts;
    uint16_t sog_config;
    uint16_t freq_tolerance;
};

/* UPS context — holds connection and active driver.
 * The cmd_mutex serializes all command writes (freq tolerance, bypass, etc.)
 * so concurrent callers (API thread, weather thread) don't collide. */
typedef struct ups_context {
    modbus_t           *ctx;
    const ups_driver_t *driver;
    ups_inventory_t     inventory;  /* cached at connect time */
    int                 has_inventory;
    pthread_mutex_t     cmd_mutex;  /* serializes command writes */
    int                 consecutive_errors;
    /* Connection params for reconnect */
    char                device[256];
    int                 baud;
    int                 slave_id;
} ups_t;

/* Connect and auto-detect driver from UPS model string.
 * Returns NULL on connection failure or unknown model. */
ups_t *ups_connect(const char *device, int baud, int slave_id);
void   ups_close(ups_t *ups);

/* Query capabilities */
int ups_has_cap(const ups_t *ups, ups_cap_t cap);
const char *ups_driver_name(const ups_t *ups);

/* Frequency tolerance settings from the active driver */
const ups_freq_setting_t *ups_get_freq_settings(const ups_t *ups, size_t *count);
const ups_freq_setting_t *ups_find_freq_setting(const ups_t *ups, const char *name);
const ups_freq_setting_t *ups_find_freq_value(const ups_t *ups, uint16_t value);

/* Reads — return 0 on success, negative on error */
int ups_read_status(ups_t *ups, ups_data_t *data);
int ups_read_dynamic(ups_t *ups, ups_data_t *data);
int ups_read_inventory(ups_t *ups, ups_inventory_t *inv);
int ups_read_thresholds(ups_t *ups, uint16_t *transfer_high, uint16_t *transfer_low);

/* Commands — return 0 on success, UPS_ERR_NOT_SUPPORTED if driver
 * doesn't implement it, negative on I/O error */
int ups_cmd_shutdown(ups_t *ups);
int ups_cmd_battery_test(ups_t *ups);
int ups_cmd_runtime_cal(ups_t *ups);
int ups_cmd_abort_runtime_cal(ups_t *ups);
int ups_cmd_clear_faults(ups_t *ups);
int ups_cmd_mute_alarm(ups_t *ups);
int ups_cmd_cancel_mute(ups_t *ups);
int ups_cmd_beep_test(ups_t *ups);
int ups_cmd_bypass_enable(ups_t *ups);
int ups_cmd_bypass_disable(ups_t *ups);
int ups_cmd_set_freq_tolerance(ups_t *ups, uint16_t setting);

/* --- Config register access --- */

/* Get the list of config register descriptors from the active driver. */
const ups_config_reg_t *ups_get_config_regs(const ups_t *ups, size_t *count);

/* Find a config register descriptor by name. Returns NULL if not found. */
const ups_config_reg_t *ups_find_config_reg(const ups_t *ups, const char *name);

/* Read a config register value. Returns raw uint16 value(s).
 * For scalars: *value = raw register value (caller applies scale).
 * For strings: writes decoded string to str_buf.
 * Returns 0 on success. */
int ups_config_read(ups_t *ups, const ups_config_reg_t *reg,
                    uint16_t *raw_value, char *str_buf, size_t str_bufsz);

/* Write a config register value. Enforces 100ms inter-write delay.
 * Reads back after write for verification.
 * Returns 0 on success, UPS_ERR_NOT_SUPPORTED if not writable. */
int ups_config_write(ups_t *ups, const ups_config_reg_t *reg, uint16_t value);

/* Human-readable strings (shared across all APC Modbus models) */
const char *ups_transfer_reason_str(uint16_t reason);
const char *ups_status_str(uint32_t status, char *buf, size_t len);
const char *ups_efficiency_str(int16_t raw, char *buf, size_t len);

#endif
