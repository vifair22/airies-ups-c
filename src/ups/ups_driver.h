#ifndef UPS_DRIVER_H
#define UPS_DRIVER_H

#include <stdint.h>
#include <stddef.h>
#include <modbus/modbus.h>

/* Forward declarations */
typedef struct ups_data ups_data_t;
typedef struct ups_inventory ups_inventory_t;

/* Capability flags — driver advertises what it supports */
typedef enum {
    UPS_CAP_SHUTDOWN       = (1 << 0),
    UPS_CAP_BATTERY_TEST   = (1 << 1),
    UPS_CAP_RUNTIME_CAL    = (1 << 2),
    UPS_CAP_CLEAR_FAULTS   = (1 << 3),
    UPS_CAP_MUTE           = (1 << 4),
    UPS_CAP_BEEP           = (1 << 5),
    UPS_CAP_BYPASS         = (1 << 6),
    UPS_CAP_FREQ_TOLERANCE = (1 << 7),
    UPS_CAP_HE_MODE        = (1 << 8),
} ups_cap_t;

/* Frequency tolerance setting descriptor.
 * Each driver provides an array of these describing valid settings. */
typedef struct {
    uint16_t    value;        /* register value to write */
    const char *name;         /* short name: "hz60_0_1" */
    const char *description;  /* human-readable: "60 Hz +/- 0.1 Hz" */
    int         inhibits_he;  /* 1 if this tolerance inhibits HE mode */
} ups_freq_setting_t;

/* Config register types */
typedef enum {
    UPS_CFG_SCALAR,     /* uint16, read as integer, apply scale */
    UPS_CFG_BITFIELD,   /* uint16, one-bit-set enum */
    UPS_CFG_STRING,     /* multiple regs, 2 ASCII chars per reg */
} ups_cfg_type_t;

/* Bitfield option descriptor */
typedef struct {
    uint16_t    value;  /* register value (single bit set) */
    const char *name;   /* short name: "onstart_plus_7" */
    const char *label;  /* display: "On startup + every 7 days" */
} ups_bitfield_opt_t;

/* Config register descriptor */
typedef struct {
    const char    *name;         /* "transfer_high" */
    const char    *display_name; /* "Input Transfer High Voltage" */
    const char    *unit;         /* "V", "s", "%", NULL */
    const char    *group;        /* "transfer", "delays", "load_shed" */
    uint16_t       reg_addr;
    uint8_t        reg_count;    /* 1 for uint16, 2 for uint32, N for string */
    ups_cfg_type_t type;
    uint16_t       scale;        /* divisor (1 = raw, 0 = raw) */
    int            writable;

    /* Type-specific metadata */
    union {
        struct { int32_t min; int32_t max; } scalar;
        struct { const ups_bitfield_opt_t *opts; size_t count; } bitfield;
        struct { size_t max_chars; } string;
    } meta;
} ups_config_reg_t;

/* Driver vtable — each UPS family implements this */
typedef struct ups_driver {
    const char *name;  /* "srt", "smt" */

    /* Detection: return 1 if this driver handles the given model string */
    int (*detect)(const char *model);

    /* Capability bitfield */
    uint32_t caps;

    /* Frequency tolerance settings (NULL if UPS_CAP_FREQ_TOLERANCE not set) */
    const ups_freq_setting_t *freq_settings;
    size_t                    freq_settings_count;

    /* Config register descriptors (NULL if none) */
    const ups_config_reg_t *config_regs;
    size_t                  config_regs_count;

    /* --- Reads --- */
    int (*read_status)(modbus_t *ctx, ups_data_t *data);
    int (*read_dynamic)(modbus_t *ctx, ups_data_t *data);
    int (*read_inventory)(modbus_t *ctx, ups_inventory_t *inv);
    int (*read_thresholds)(modbus_t *ctx, uint16_t *transfer_high, uint16_t *transfer_low);

    /* --- Commands --- */
    int (*cmd_shutdown)(modbus_t *ctx);
    int (*cmd_battery_test)(modbus_t *ctx);
    int (*cmd_runtime_cal)(modbus_t *ctx);
    int (*cmd_abort_runtime_cal)(modbus_t *ctx);
    int (*cmd_clear_faults)(modbus_t *ctx);
    int (*cmd_mute_alarm)(modbus_t *ctx);
    int (*cmd_cancel_mute)(modbus_t *ctx);
    int (*cmd_beep_test)(modbus_t *ctx);
    int (*cmd_bypass_enable)(modbus_t *ctx);
    int (*cmd_bypass_disable)(modbus_t *ctx);
    int (*cmd_set_freq_tolerance)(modbus_t *ctx, uint16_t setting);
} ups_driver_t;

/* Driver registry — NULL-terminated array of available drivers.
 * Defined in ups.c, populated by including each driver's extern. */
extern const ups_driver_t *ups_drivers[];

#endif
