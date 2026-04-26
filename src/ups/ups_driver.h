#ifndef UPS_DRIVER_H
#define UPS_DRIVER_H

#include <stdint.h>
#include <stddef.h>

/* ============================================================================
 *  UPS Driver API — contract for implementing a new UPS family
 * ============================================================================
 *
 * A driver is a static `const ups_driver_t` (see bottom of this file) that
 * teaches the registry how to open a transport, identify a UPS, and exchange
 * data. The daemon uses the driver through the registry in ups.h; drivers
 * never talk directly to the daemon.
 *
 *   Lifecycle, per successful connect:
 *     1. ups_connect() iterates drivers matching params->type
 *     2. drv->connect(params) opens a transport and returns an opaque handle,
 *        or NULL if the physical layer is unavailable
 *     3. drv->detect(transport) returns 1 if this driver claims the UPS
 *     4. (optional) drv->get_topology(transport) overrides the static field
 *     5. (optional) drv->resolve_caps(transport, drv->caps) narrows caps to
 *        what actually resolved on this device
 *     6. ups_read_inventory() is called once to populate ups->inventory
 *     7. Monitor thread begins calling drv->read_status / read_dynamic on a
 *        fixed cadence; API routes call drv->execute and drv->config_write
 *        as operator actions arrive
 *     8. On shutdown, drv->disconnect(transport) releases the handle
 *
 *   Thread-safety contract (enforced by the registry):
 *     - Every call into any driver callback is made with ups->cmd_mutex held,
 *       i.e. callbacks see strictly single-threaded access to `transport`
 *     - Drivers MUST NOT spawn their own threads that touch transport; if
 *       the driver needs asynchrony, it must self-serialize internally
 *     - After every successful command execute or config_write, the registry
 *       sleeps 200 ms before releasing the mutex — drivers can rely on this
 *       quiet window for UPS firmware to process the write
 *
 *   Error convention:
 *     - All int-returning driver callbacks return 0 on success, negative
 *       (typically -1) on failure. The registry maps that to UPS_ERR_IO for
 *       the public-API caller.
 *
 *   Memory ownership:
 *     - `transport`: allocated by drv->connect, freed by drv->disconnect
 *     - `ups_conn_params_t.*`: strings are copied into ups_t at connect time,
 *       so callers need not keep the originals alive
 *     - Static tables (freq_settings, config_regs, commands) are owned by
 *       the driver translation unit; the registry never modifies them
 * ============================================================================ */

/* Forward declarations; real definitions live in ups.h so that this header
 * stays free of the status-bit and error-code macros the daemon consumes. */
typedef struct ups_data       ups_data_t;
typedef struct ups_inventory  ups_inventory_t;

/* ---------------------------------------------------------------------------
 *  Connection transport
 * --------------------------------------------------------------------------- */

/* Transport category — picked at config time, narrows which drivers the
 * registry considers for this UPS. Two drivers with different conn_types
 * never compete for the same physical UPS.
 *
 * `UPS_CONN_TCP` and `UPS_CONN_SNMP` are placeholders: the enum values and
 * params slots are defined so future drivers can plug in without a type
 * change, but no driver currently implements either. Attempting to connect
 * with one of these today yields UPS_ERR_NO_DRIVER. */
typedef enum {
    UPS_CONN_SERIAL,    /* RS-232/RS-485 — Modbus RTU, APC UPS-Link */
    UPS_CONN_USB,       /* USB HID Power Device Class */
    UPS_CONN_TCP,       /* Modbus TCP (placeholder, no driver yet) */
    UPS_CONN_SNMP,      /* SNMP via NMC (placeholder, no driver yet) */
} ups_conn_type_t;

/* Connection parameters — the active union branch is selected by `type`.
 *
 * All `const char *` fields are copied into ups_t at connect time
 * (see _device_buf / _serial_buf / _host_buf in ups_context). Callers do
 * not need to keep the originals alive beyond the ups_connect() call. */
typedef struct {
    ups_conn_type_t type;
    union {
        /* RS-232/RS-485 serial — APC SRT / SMT over Modbus RTU.
         *   device   : tty path (e.g. "/dev/ttyUSB0" or by-id)
         *   baud     : typically 9600 for APC Modbus RTU
         *   slave_id : Modbus unit ID (1..247); APC default is 1 */
        struct { const char *device; int baud; int slave_id; } serial;

        /* USB HID — APC Back-UPS, USB-attached Smart-UPS.
         *   vendor_id / product_id : USB IDs (e.g., 0x051d / 0x0002 for APC)
         *   serial                 : iSerial descriptor filter, or NULL to
         *                            match the first device with matching VID/PID */
        struct { uint16_t vendor_id; uint16_t product_id; const char *serial; } usb;

        /* Modbus TCP — placeholder.
         *   slave_id corresponds to Modbus unit ID. */
        struct { const char *host; int port; int slave_id; } tcp;

        /* SNMP via APC NMC — placeholder.
         *   version 1 or 2 today; v3 authentication not yet modeled. */
        struct { const char *host; int port; const char *community; int version; } snmp;
    };
} ups_conn_params_t;

/* ---------------------------------------------------------------------------
 *  UPS topology
 * --------------------------------------------------------------------------- */

/* Power-path topology as reported by the driver. The UI uses this to tune
 * what it shows (e.g. hiding bypass-related widgets for standby UPSes). */
typedef enum {
    UPS_TOPO_ONLINE_DOUBLE,     /* full double-conversion (SRT) */
    UPS_TOPO_LINE_INTERACTIVE,  /* AVR / line-interactive (SMT, some Back-UPS) */
    UPS_TOPO_STANDBY,           /* standby / offline (Back-UPS) */
} ups_topology_t;

/* ---------------------------------------------------------------------------
 *  Capabilities
 * --------------------------------------------------------------------------- */

/* Feature flags a driver may advertise. Each flag gates a distinct subset
 * of the public API: e.g., `UPS_CAP_SHUTDOWN` unlocks the shutdown command
 * in the UI and allows the shutdown orchestrator to trigger it.
 *
 * Drivers set these on `ups_driver_t.caps` as their maximum set, and may
 * narrow them at connect time via `resolve_caps` based on what resolved
 * against the live device. Consumers query the effective set using
 * `ups_has_cap(ups, UPS_CAP_*)` which reads from ups->caps (not driver->caps),
 * so capability queries are always honest. */
typedef enum {
    UPS_CAP_SHUTDOWN       = (1 << 0),  /* execute shutdown (required by shutdown orchestrator) */
    UPS_CAP_BATTERY_TEST   = (1 << 1),  /* self-test battery */
    UPS_CAP_RUNTIME_CAL    = (1 << 2),  /* deep-discharge runtime recalibration */
    UPS_CAP_CLEAR_FAULTS   = (1 << 3),  /* clear latched fault indicators */
    UPS_CAP_MUTE           = (1 << 4),  /* silence audible alarm */
    UPS_CAP_BEEP           = (1 << 5),  /* trigger a short beep test */
    UPS_CAP_BYPASS         = (1 << 6),  /* toggle manual bypass */
    UPS_CAP_FREQ_TOLERANCE = (1 << 7),  /* frequency tolerance configurable (requires freq_settings) */
    UPS_CAP_HE_MODE        = (1 << 8),  /* high-efficiency / green mode available */
} ups_cap_t;

/* ---------------------------------------------------------------------------
 *  Frequency tolerance settings
 * --------------------------------------------------------------------------- */

/* One valid frequency-tolerance choice for the UPS. The driver declares
 * which settings the hardware accepts; the UI renders them as a dropdown.
 *
 * Active only when the driver advertises `UPS_CAP_FREQ_TOLERANCE`. */
typedef struct {
    uint16_t    value;        /* register value to write */
    const char *name;         /* short name for API (e.g. "hz60_0_1") */
    const char *description;  /* human-readable label ("60 Hz +/- 0.1 Hz") */
} ups_freq_setting_t;

/* ---------------------------------------------------------------------------
 *  Config registers — spec-driven read/write of UPS settings
 * --------------------------------------------------------------------------- */

/* How the app should decode a config register's raw value. */
typedef enum {
    UPS_CFG_SCALAR,     /* unsigned integer; apply `scale` divisor to display */
    UPS_CFG_BITFIELD,   /* one-bit-set enum; look up the matching option */
    UPS_CFG_STRING,     /* multiple registers, 2 ASCII chars per register */
    UPS_CFG_FLAGS,      /* multi-bit field; decode every set bit via opts[] */
} ups_cfg_type_t;

/* Semantic class of a register descriptor. The /api/config/ups endpoint
 * filters to UPS_REG_CATEGORY_CONFIG so the operator-facing settings page
 * isn't flooded with read-only diagnostics. /api/about returns every
 * category. Default 0 = CONFIG so existing entries don't need annotation. */
typedef enum {
    UPS_REG_CATEGORY_CONFIG = 0,    /* tunable / operator-facing setting */
    UPS_REG_CATEGORY_MEASUREMENT,   /* live numeric reading (voltage, load, etc.) */
    UPS_REG_CATEGORY_IDENTITY,      /* static device fact (model, serial, ratings) */
    UPS_REG_CATEGORY_DIAGNOSTIC,    /* status bits, error flags, test progress */
} ups_reg_category_t;

/* One named option for a BITFIELD config register.
 *
 * The set of opts[] is treated as the authoritative list of values
 * accepted by the UPS firmware on this model. Values NOT in the list
 * may be spec-defined but rejected in practice (see e.g. SRT bat_test
 * where the firmware rejects OnStartUpPlus7 / OnStartUpPlus14 even
 * though AN-176 defines them) — driver comments should explain any
 * such omissions so future maintainers don't "fix" them by adding the
 * rejected values back. When `bitfield.strict` is set on the
 * descriptor, the registry blocks writes of any value not in opts[]. */
typedef struct {
    uint32_t    value;  /* raw register value or bit mask. uint32 so that
                         * UPS_CFG_FLAGS descriptors against 32-bit
                         * registers (UPSStatus, PowerSystemError, outlet
                         * group status) can name bits beyond bit 15. */
    const char *name;   /* API key (e.g. "onstart_plus_7") */
    const char *label;  /* display ("On startup + every 7 days") */
} ups_bitfield_opt_t;

/* Config register descriptor.
 *
 * Each driver provides a static array of these so the daemon and UI can
 * render settings generically without knowing the register layout of each
 * UPS family. The API surfaces them under `/config/<name>`.
 *
 * `reg_addr` is driver-defined: Modbus drivers use it as an absolute register
 * address, while the HID driver uses it as an opaque field-lookup key that
 * `resolve_fields` translates into a live HID field pointer. */
typedef struct {
    const char    *name;         /* API key (e.g. "transfer_high") */
    const char    *display_name; /* UI label ("Input Transfer High Voltage") */
    const char    *unit;         /* "V", "s", "%", NULL */
    const char    *group;        /* UI grouping ("transfer", "delays") */
    uint16_t       reg_addr;     /* driver-interpreted register/field id */
    uint8_t        reg_count;    /* 1 for uint16, 2 for uint32, N for string */
    ups_cfg_type_t type;
    uint16_t       scale;        /* divisor for display (1 = raw) */
    int            writable;     /* non-zero if driver supports writing */

    /* Type-specific metadata. Access the union branch that matches `type`.
     *
     * Validation contract (enforced by ups_config_write before the driver
     * sees the value, returns UPS_ERR_INVALID_VALUE on failure):
     *   SCALAR   — value must satisfy `min <= value <= max` when min/max
     *              are set. The convention `min == 0 && max == 0` means
     *              "no range constraint declared" and skips the check.
     *   BITFIELD — when `strict` is non-zero, value must equal one of the
     *              entries in `opts[]`. When `strict` is zero, any value
     *              passes through (use sparingly — only when the driver
     *              accepts arbitrary bit combinations or raw bypass).
     *   STRING   — max_chars is enforced by the string-write path.
     *
     * Hints (UI-only, no enforcement):
     *   scalar.step — preferred increment for spinners and sliders
     *                 (frontend uses it as <input step=N>); 0 = no
     *                 declared step. The wire still accepts any in-range
     *                 integer; step is purely a UX hint. */
    union {
        struct {
            int32_t min;
            int32_t max;
            int32_t step;     /* UI hint, 0 = no declared step */
        } scalar;             /* UPS_CFG_SCALAR */
        struct {
            const ups_bitfield_opt_t *opts;
            size_t                    count;
            int                       strict;  /* 1 = reject writes not in opts[] */
        } bitfield;           /* UPS_CFG_BITFIELD and UPS_CFG_FLAGS */
        struct {
            size_t max_chars; /* enforced on string writes */
        } string;             /* UPS_CFG_STRING */
    } meta;

    /* Appended fields. Default-zero is safe for every existing entry: 0 =
     * CONFIG category and no sentinel. Append-only here keeps positional
     * initializers in the existing srt_config_regs[] / smt_config_regs[] /
     * backups_config_regs[] tables valid; new entries should use designated
     * initializers when they need to set these. */
    ups_reg_category_t category;       /* default 0 = CONFIG */
    int                has_sentinel;   /* 1 = sentinel_value means "N/A" */
    uint32_t           sentinel_value; /* compared against the raw read value */
} ups_config_reg_t;

/* ---------------------------------------------------------------------------
 *  Commands — driver-declared operator actions
 * --------------------------------------------------------------------------- */

/* Two command shapes:
 *   SIMPLE — one-shot action (e.g. "trigger self-test"), one execute handler
 *   TOGGLE — on/off pair; state read from `status_bit` in ups_data.status */
typedef enum {
    UPS_CMD_SIMPLE,
    UPS_CMD_TOGGLE,
} ups_cmd_type_t;

/* Visual weight of the command button in the UI. */
typedef enum {
    UPS_CMD_DEFAULT,    /* neutral styling */
    UPS_CMD_WARN,       /* amber — "this interrupts service" */
    UPS_CMD_DANGER,     /* red — "this cuts power" */
} ups_cmd_variant_t;

/* Command flag bits. Used by orchestrators to locate the right command
 * without string-matching on `name`. */
#define UPS_CMD_IS_SHUTDOWN  (1 << 0)  /* shutdown orchestrator targets this command */
#define UPS_CMD_IS_MUTE      (1 << 1)  /* mute command (used to silence continuous beep) */

/* Command descriptor. Drivers provide a static array; the API enumerates
 * them for the UI and dispatches via `ups_cmd_execute()` by name. */
typedef struct {
    const char       *name;           /* API dispatch key (e.g. "battery_test") */
    const char       *display_name;   /* button label */
    const char       *description;    /* row help text */
    const char       *group;          /* UI grouping ("power", "diagnostics", "alarm") */
    const char       *confirm_title;  /* modal title, NULL to skip confirm */
    const char       *confirm_body;   /* modal body text, NULL to skip */
    ups_cmd_type_t    type;
    ups_cmd_variant_t variant;
    uint32_t          flags;          /* UPS_CMD_IS_* bits */
    uint32_t          status_bit;     /* TOGGLE: which bit of ups_data.status reflects "on" */

    /* Handlers. Called under ups->cmd_mutex. Return 0 on success, -1 on error.
     * For TOGGLE commands, execute() fires the "on" action and execute_off()
     * the "off" action; SIMPLE commands leave execute_off NULL. */
    int (*execute)(void *transport);
    int (*execute_off)(void *transport);
} ups_cmd_desc_t;

/* ---------------------------------------------------------------------------
 *  Driver vtable — the actual contract
 * --------------------------------------------------------------------------- */

/* Each UPS family implements this struct as a static const, and ups.c
 * includes a pointer to it in the NULL-terminated `ups_drivers[]` registry.
 *
 * The `void *transport` parameter throughout is an opaque handle allocated
 * by connect(). The daemon never inspects or frees it — disconnect() owns
 * the lifetime. Drivers cast transport to whatever their transport library
 * needs (modbus_t *, hid_transport_t *, socket fd, etc.).
 *
 * See the file-level "Thread-safety contract" block above — all callbacks
 * are serialized via ups->cmd_mutex, and drivers must not spawn threads
 * that touch transport. */
typedef struct ups_driver {
    /* ---- Identity ---- */
    const char      *name;      /* stable driver id: "srt", "smt", "backups_hid" */
    ups_conn_type_t  conn_type; /* transport category this driver expects */
    ups_topology_t   topology;  /* default topology; may be overridden by get_topology */

    /* ---- Connection lifecycle ----
     *
     * connect(): open a transport and return the opaque handle. Return NULL
     *            on I/O failure. The daemon calls this speculatively across
     *            multiple drivers, so it must be side-effect-light and must
     *            not leave the device in a weird state on failure.
     *
     * disconnect(): release the handle. Called exactly once per successful
     *               connect(), on either detect-miss or shutdown. */
    void *(*connect)(const ups_conn_params_t *params);
    void  (*disconnect)(void *transport);

    /* ---- Identification ----
     *
     * detect(): return 1 if this driver recognises the device on the live
     *           transport, 0 otherwise. Called immediately after connect();
     *           transport is open. Drivers typically read a model/SKU
     *           register or USB descriptor and substring-match. Keep the
     *           probe small (one or two reads) — it runs on every connect
     *           candidate.
     *
     * get_topology(): optional override. If set, called after detect() wins
     *                 and the return value replaces the static topology
     *                 field on this connection. Useful when a single driver
     *                 serves multiple topologies (e.g. Back-UPS: standby or
     *                 line-interactive depending on PowerConverter
     *                 collection presence). */
    int            (*detect)(void *transport);
    ups_topology_t (*get_topology)(void *transport);  /* optional (NULL = use static) */

    /* ---- Capabilities ----
     *
     * caps: the maximum capability set this driver may expose.
     *
     * resolve_caps(): optional. Called after detect() succeeds; returns the
     *                 effective capability mask for this live connection
     *                 (usually default_caps with some bits cleared based on
     *                 what probed successfully). If NULL, driver->caps is
     *                 used verbatim. The result is stored on ups_t and read
     *                 by ups_has_cap(). */
    uint32_t  caps;
    uint32_t (*resolve_caps)(void *transport, uint32_t default_caps);  /* optional */

    /* ---- Static tables ----
     *
     * freq_settings : NULL unless UPS_CAP_FREQ_TOLERANCE is in caps
     * config_regs   : NULL if the driver exposes no settings
     * commands      : NULL if the driver exposes no commands */
    const ups_freq_setting_t *freq_settings;
    size_t                    freq_settings_count;

    const ups_config_reg_t   *config_regs;
    size_t                    config_regs_count;

    /* Optional: narrow the config_regs set at connect time.
     *
     * Parallel to resolve_caps. Useful when the driver's static
     * config_regs[] array includes descriptors whose backing register /
     * HID field may not be present on every device in the family (e.g.
     * Back-UPS models with differing HID report coverage).
     *
     * Writes up to default_count struct copies into out[] and returns the
     * number actually written. The registry allocates out with capacity
     * default_count before calling, and stores the resulting subset on
     * ups_t for ups_get_config_regs to return.
     *
     * If NULL, the registry exposes driver->config_regs verbatim. */
    size_t (*resolve_config_regs)(void *transport,
                                  const ups_config_reg_t *default_regs,
                                  size_t default_count,
                                  ups_config_reg_t *out);

    /* ---- Reads ----
     *
     * All readers return 0 on success, -1 on I/O failure. Called under the
     * cmd_mutex. Each function owns the layout of its target struct —
     * fields not read should be left zeroed (the daemon zeroes before the
     * first call).
     *
     * read_status:     fast, high-frequency (every poll). Populates status
     *                  bits, outlet states, error registers — anything that
     *                  changes on event boundaries.
     * read_dynamic:    fast, high-frequency. Populates measurements (V, A,
     *                  Hz, load%, runtime, charge%, efficiency).
     * read_inventory:  slow, called once at connect time. Populates model,
     *                  serial, firmware, nominal ratings. The result is
     *                  cached on ups_t->inventory.
     * read_thresholds: low-frequency. Returns the input-voltage transfer
     *                  thresholds in VOLTS (not raw register counts).
     *                  Drivers must pre-scale if the underlying register
     *                  stores some other unit. Daemon caches and rarely
     *                  re-reads. */
    int (*read_status)    (void *transport, ups_data_t *data);
    int (*read_dynamic)   (void *transport, ups_data_t *data);
    int (*read_inventory) (void *transport, ups_inventory_t *inv);
    int (*read_thresholds)(void *transport, uint16_t *transfer_high, uint16_t *transfer_low);

    /* ---- Commands ----
     *
     * See ups_cmd_desc_t. Dispatched by name via ups_cmd_execute(). */
    const ups_cmd_desc_t *commands;
    size_t                commands_count;

    /* ---- Config register I/O ----
     *
     * Required only when config_regs is non-NULL.
     *
     * config_read: decode one descriptor. For scalar/bitfield, write the
     *              raw register value into *raw_value. For string, write
     *              a NUL-terminated ASCII string into str_buf (respecting
     *              str_bufsz). Return 0 on success, -1 on I/O failure.
     *
     * config_write: write one descriptor. The registry pre-checks
     *               reg->writable; drivers may assume the descriptor is
     *               writable when this fires. Return 0 on success, -1 on
     *               failure. The registry guarantees a 100 ms inter-write
     *               delay and reads the value back afterwards. */
    int (*config_read)(void *transport, const ups_config_reg_t *reg,
                       uint32_t *raw_value, char *str_buf, size_t str_bufsz);
    int (*config_write)(void *transport, const ups_config_reg_t *reg, uint16_t value);
} ups_driver_t;

/* ---------------------------------------------------------------------------
 *  Driver registry
 * --------------------------------------------------------------------------- */

/* NULL-terminated array of registered drivers, defined in ups.c. Iteration
 * order determines detect precedence — the first driver whose detect()
 * returns 1 wins. Put more specific drivers (e.g., SRT) before more
 * permissive ones (e.g., generic HID) to avoid misclassification. */
extern const ups_driver_t *ups_drivers[];

#endif
