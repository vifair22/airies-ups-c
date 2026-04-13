#ifndef UPS_H
#define UPS_H

#include <stdint.h>
#include <modbus/modbus.h>

/* Status bits (reg 0-1, uint32) */
#define UPS_ST_ONLINE       (1 << 1)
#define UPS_ST_ON_BATTERY   (1 << 2)
#define UPS_ST_BYPASS       (1 << 3)
#define UPS_ST_OUTPUT_OFF   (1 << 4)
#define UPS_ST_FAULT        (1 << 5)
#define UPS_ST_INPUT_BAD    (1 << 6)
#define UPS_ST_TEST         (1 << 7)
#define UPS_ST_SHUT_PENDING (1 << 9)
#define UPS_ST_HE_MODE      (1 << 13)
#define UPS_ST_OVERLOAD     (1 << 21)

/* Sig register 18 bits */
#define UPS_SIG_ACTIVE          (1 << 0)
#define UPS_SIG_SHUTDOWN_IMMINENT (1 << 1)

/* Battery error register 19 bits */
#define UPS_BATERR_REPLACE  (1 << 1)

/* Parsed UPS status */
typedef struct {
    /* Status block (reg 0-26) */
    uint32_t status;
    uint16_t transfer_reason;
    uint32_t outlet_mog;
    uint32_t outlet_sog0;
    uint32_t outlet_sog1;
    uint16_t sig_status;
    uint16_t bat_error;
    uint16_t bat_test_status;
    uint16_t rt_cal_status;

    /* Dynamic block (reg 128-171) */
    uint32_t runtime_sec;
    double   charge_pct;
    double   battery_voltage;
    double   load_pct;
    double   output_current;
    double   output_voltage;
    double   output_frequency;
    uint32_t output_energy_wh;
    double   input_voltage;
    double   efficiency;
    int16_t  timer_shutdown;
    int16_t  timer_start;
    int32_t  timer_reboot;
} ups_data_t;

/* Inventory (read once) */
typedef struct {
    char     model[33];
    char     serial[17];
    char     firmware[17];
    uint16_t nominal_va;
    uint16_t nominal_watts;
    uint16_t sog_config;
    uint16_t operating_mode;
} ups_inventory_t;

/* Connection */
modbus_t *ups_connect(const char *device, int baud, int slave_id);
void      ups_close(modbus_t *ctx);

/* Reads — return 0 on success */
int ups_read_status(modbus_t *ctx, ups_data_t *data);
int ups_read_dynamic(modbus_t *ctx, ups_data_t *data);
int ups_read_inventory(modbus_t *ctx, ups_inventory_t *inv);

/* Commands — return 0 on success */
int ups_cmd_shutdown(modbus_t *ctx);
int ups_cmd_clear_faults(modbus_t *ctx);
int ups_cmd_battery_test(modbus_t *ctx);
int ups_cmd_mute_alarm(modbus_t *ctx);
int ups_cmd_cancel_mute(modbus_t *ctx);
int ups_cmd_beep_test(modbus_t *ctx);
int ups_cmd_bypass_enable(modbus_t *ctx);
int ups_cmd_bypass_disable(modbus_t *ctx);
int ups_cmd_set_mode(modbus_t *ctx, uint16_t mode);

/* Read transfer voltage thresholds (regs 1026-1027) */
int ups_read_thresholds(modbus_t *ctx, uint16_t *transfer_high, uint16_t *transfer_low);

/* Human-readable strings */
const char *ups_transfer_reason_str(uint16_t reason);
const char *ups_status_str(uint32_t status, char *buf, size_t len);
const char *ups_efficiency_str(int16_t raw, char *buf, size_t len);

#endif
