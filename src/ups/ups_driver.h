#ifndef UPS_DRIVER_H
#define UPS_DRIVER_H

#include <stdint.h>
#include <stddef.h>

/* Forward declarations */
typedef struct ups_data ups_data_t;
typedef struct ups_inventory ups_inventory_t;

/* Connection types */
typedef enum {
    UPS_CONN_SERIAL,    /* RS-232/RS-485 (Modbus RTU, etc.) */
    UPS_CONN_USB,       /* USB HID */
    UPS_CONN_TCP,       /* Modbus TCP, SNMP, vendor HTTP (future) */
} ups_conn_type_t;

/* Connection parameters — driver interprets based on conn_type */
typedef struct {
    ups_conn_type_t type;
    union {
        struct { const char *device; int baud; int slave_id; } serial;
        struct { uint16_t vendor_id; uint16_t product_id; const char *serial; } usb;
        struct { const char *host; int port; } tcp;
    };
} ups_conn_params_t;

/* UPS topology — reported by driver, shapes the UI */
typedef enum {
    UPS_TOPO_ONLINE_DOUBLE,     /* full double-conversion (SRT) */
    UPS_TOPO_LINE_INTERACTIVE,  /* AVR / line-interactive (SMT) */
    UPS_TOPO_STANDBY,           /* standby / offline (Back-UPS) */
} ups_topology_t;

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

/* Command descriptor — driver declares available commands.
 * The app renders UI from these and dispatches by name. */

typedef enum {
    UPS_CMD_SIMPLE,     /* fire-and-forget, one handler */
    UPS_CMD_TOGGLE,     /* on/off pair, state tracked by status_bit */
} ups_cmd_type_t;

typedef enum {
    UPS_CMD_DEFAULT,    /* normal styling */
    UPS_CMD_WARN,       /* yellow/warning styling */
    UPS_CMD_DANGER,     /* red/danger styling */
} ups_cmd_variant_t;

#define UPS_CMD_IS_SHUTDOWN  (1 << 0)  /* shutdown orchestrator uses this command */
#define UPS_CMD_IS_MUTE      (1 << 1)  /* mute command (used to stop continuous beep) */

typedef struct {
    const char       *name;           /* API dispatch key: "battery_test" */
    const char       *display_name;   /* button label: "Battery Test" */
    const char       *description;    /* row help text */
    const char       *group;          /* "power", "diagnostics", "alarm" */
    const char       *confirm_title;  /* modal title */
    const char       *confirm_body;   /* modal body text */
    ups_cmd_type_t    type;
    ups_cmd_variant_t variant;
    uint32_t          flags;          /* UPS_CMD_IS_SHUTDOWN, etc. */
    uint32_t          status_bit;     /* for toggles: bit in ups_data.status */

    /* Handlers */
    int (*execute)(void *transport);
    int (*execute_off)(void *transport);  /* toggle: the "off/disable" action */
} ups_cmd_desc_t;

/* Driver vtable — each UPS family implements this.
 *
 * The void *transport parameter is an opaque handle allocated by the driver's
 * connect() function. The driver casts it to whatever its transport library
 * needs (modbus_t *, hid_device *, socket fd, etc.). The app layer never
 * inspects or frees it — disconnect() handles cleanup.
 *
 * Thread safety: the app holds a mutex around every call into the driver.
 * Driver functions can assume single-threaded access. */
typedef struct ups_driver {
    const char      *name;      /* "srt", "smt", "apc_hid" */
    ups_conn_type_t  conn_type; /* what transport this driver uses */
    ups_topology_t   topology;  /* power path topology */

    /* Connection lifecycle — driver owns the transport */
    void *(*connect)(const ups_conn_params_t *params);
    void  (*disconnect)(void *transport);

    /* Detection: return 1 if this driver handles the connected UPS.
     * Called after connect() succeeds. Transport is live. */
    int (*detect)(void *transport);

    /* Optional: override the static topology field per-connection.
     * If NULL, the static topology field is used. Called after connect(). */
    ups_topology_t (*get_topology)(void *transport);

    /* Capability bitfield */
    uint32_t caps;

    /* Frequency tolerance settings (NULL if UPS_CAP_FREQ_TOLERANCE not set) */
    const ups_freq_setting_t *freq_settings;
    size_t                    freq_settings_count;

    /* Config register descriptors (NULL if none) */
    const ups_config_reg_t *config_regs;
    size_t                  config_regs_count;

    /* --- Reads --- */
    int (*read_status)(void *transport, ups_data_t *data);
    int (*read_dynamic)(void *transport, ups_data_t *data);
    int (*read_inventory)(void *transport, ups_inventory_t *inv);
    int (*read_thresholds)(void *transport, uint16_t *transfer_high, uint16_t *transfer_low);

    /* --- Commands (driver-declared, dynamically rendered) --- */
    const ups_cmd_desc_t *commands;
    size_t                commands_count;

    /* --- Config register I/O (optional, NULL if no config_regs) --- */
    int (*config_read)(void *transport, const ups_config_reg_t *reg,
                       uint16_t *raw_value, char *str_buf, size_t str_bufsz);
    int (*config_write)(void *transport, const ups_config_reg_t *reg, uint16_t value);
} ups_driver_t;

/* Driver registry — NULL-terminated array of available drivers.
 * Defined in ups.c, populated by including each driver's extern. */
extern const ups_driver_t *ups_drivers[];

#endif
