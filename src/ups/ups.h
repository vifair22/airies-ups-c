#ifndef UPS_H
#define UPS_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <pthread.h>
#include "ups_driver.h"
#include "ups_format.h"

/* ============================================================================
 *  UPS public API — the surface every daemon subsystem uses
 * ============================================================================
 *
 * This header is the "front door" for the daemon, API routes, monitor thread,
 * shutdown orchestrator, and alert engine. Everything those subsystems need
 * to interact with the UPS lives here. The driver-facing contract is in
 * ups_driver.h; the two are distinct on purpose so driver authors can focus
 * on the minimal vtable they need to implement.
 *
 *   Typical flow:
 *     ups_t *ups = NULL;
 *     if (ups_connect(&params, &ups) != UPS_OK) { ... }
 *     // monitor thread calls ups_read_status / ups_read_dynamic on a loop
 *     // API routes call ups_cmd_execute, ups_config_read / write
 *     ups_close(ups);
 *
 *   Thread safety:
 *     Every function taking a ups_t* may be called from any thread; the
 *     registry serializes access through ups->cmd_mutex internally. Callers
 *     never need to hold a lock. One consequence: blocking I/O in one thread
 *     will stall pending calls from other threads until the mutex is free.
 *     After every successful command or config_write, the registry sleeps
 *     200 ms before releasing the mutex — drivers and callers can rely on
 *     that quiet window for the UPS firmware to process the write.
 *
 *   Error codes: UPS_OK / UPS_ERR_IO / UPS_ERR_NO_DRIVER / UPS_ERR_NOT_SUPPORTED.
 *   All negative on failure; UPS_OK is zero so `if (ups_read_status(...) < 0)`
 *   reads cleanly.
 * ============================================================================ */

/* ---------------------------------------------------------------------------
 *  Status & error bitfields (APC Modbus 990-9840 layout)
 *
 *  These are the bit masks the app layer uses against fields in ups_data_t.
 *  Drivers should read the UPS's native register and map bits into this
 *  common layout; the daemon never looks at raw driver register values.
 * --------------------------------------------------------------------------- */

/* UPSStatus_BF (reg 0-1, uint32) — stored in ups_data.status */
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
#define UPS_ST_AVR_BOOST       (1 << 16)  /* line-interactive: AVR boosting */
#define UPS_ST_AVR_TRIM        (1 << 17)  /* line-interactive: AVR bucking */
#define UPS_ST_MAINS_BAD       (1 << 19)  /* SRT only */
#define UPS_ST_FAULT_RECOVERY  (1 << 20)  /* SRT only */
#define UPS_ST_OVERLOAD        (1 << 21)

/* SimpleSignalingStatus_BF (reg 18) — stored in ups_data.sig_status */
#define UPS_SIG_ACTIVE            (1 << 0)
#define UPS_SIG_SHUTDOWN_IMMINENT (1 << 1)

/* GeneralError_BF (reg 19) — stored in ups_data.general_error */
#define UPS_GENERR_SITE_WIRING   (1 << 0)
#define UPS_GENERR_EEPROM        (1 << 1)
#define UPS_GENERR_AD_CONV       (1 << 2)
#define UPS_GENERR_LOGIC_PSU     (1 << 3)
#define UPS_GENERR_INTERNAL_COMM (1 << 4)
#define UPS_GENERR_UI_BUTTON     (1 << 5)
#define UPS_GENERR_EPO_ACTIVE    (1 << 7)
#define UPS_GENERR_FW_MISMATCH   (1 << 8)

/* PowerSystemError_BF (reg 20-21, uint32) — stored in ups_data.power_system_error */
#define UPS_PWRERR_OVERLOAD      (1 << 0)
#define UPS_PWRERR_SHORT_CIRCUIT (1 << 1)
#define UPS_PWRERR_OVERVOLTAGE   (1 << 2)
#define UPS_PWRERR_OVERTEMP      (1 << 4)
#define UPS_PWRERR_FAN           (1 << 10)
#define UPS_PWRERR_INVERTER      (1 << 13)

/* BatterySystemError_BF (reg 22) — stored in ups_data.bat_system_error.
 * UPS_BATERR_DISCONNECTED gates the "zero battery fields" logic in
 * ups_read_status / ups_read_dynamic — see those functions for detail. */
#define UPS_BATERR_DISCONNECTED  (1 << 0)
#define UPS_BATERR_OVERVOLTAGE   (1 << 1)
#define UPS_BATERR_REPLACE       (1 << 2)
#define UPS_BATERR_OVERTEMP_CRIT (1 << 3)
#define UPS_BATERR_CHARGER       (1 << 4)
#define UPS_BATERR_TEMP_SENSOR   (1 << 5)
#define UPS_BATERR_OVERTEMP_WARN (1 << 7)
#define UPS_BATERR_GENERAL       (1 << 8)
#define UPS_BATERR_COMM          (1 << 9)

/* ---------------------------------------------------------------------------
 *  Return codes
 * --------------------------------------------------------------------------- */

/* Return code constants. UPS_OK == 0 so `if (rc < 0)` works for all failures. */
#define UPS_OK                 0    /* success */
#define UPS_ERR_IO            -1    /* I/O failure (transport down, driver returned -1) */
#define UPS_ERR_NO_DRIVER     -2    /* no driver claimed the UPS */
#define UPS_ERR_NOT_SUPPORTED -3    /* the active driver does not implement this feature */
#define UPS_ERR_INVALID_VALUE -4    /* config write failed validation (out of range, not in
                                     * strict bitfield opts, etc.) — caller error, not driver
                                     * error; API should map to HTTP 400 */

/* ---------------------------------------------------------------------------
 *  Efficiency encoding
 * --------------------------------------------------------------------------- */

/* APC reports instantaneous efficiency as a signed integer where positive
 * values are "percent * 128" and a handful of negative values are specific
 * reason codes for "no efficiency reading available right now" (load too
 * low to measure, UPS on battery, etc.). Rather than leak that encoding
 * into every consumer, drivers split the raw value into two fields of
 * ups_data_t:
 *
 *   efficiency_reason : one of the ups_eff_reason_t values below
 *   efficiency        : valid pct in [0, 100] iff efficiency_reason == UPS_EFF_OK
 *
 * Consumers that sort or compare efficiency must first check
 * `efficiency_reason == UPS_EFF_OK`, otherwise `efficiency` is 0.0 and
 * comparisons are meaningless (see shutdown.c for how triggers gate this).
 *
 * The non-zero values match APC's wire encoding (negated), so a driver
 * reading a negative raw value can simply `assign (ups_eff_reason_t)(-raw)`. */
typedef enum {
    UPS_EFF_OK                   = 0,  /* efficiency field holds a valid percent */
    UPS_EFF_NOT_AVAILABLE        = 1,
    UPS_EFF_LOAD_TOO_LOW         = 2,
    UPS_EFF_OUTPUT_OFF           = 3,
    UPS_EFF_ON_BATTERY           = 4,
    UPS_EFF_IN_BYPASS            = 5,
    UPS_EFF_BATTERY_CHARGING     = 6,
    UPS_EFF_POOR_AC_INPUT        = 7,
    UPS_EFF_BATTERY_DISCONNECTED = 8,
} ups_eff_reason_t;

/* ---------------------------------------------------------------------------
 *  ups_data_t — per-poll snapshot populated by read_status / read_dynamic
 *
 *  Drivers clear only the fields they populate; callers should treat
 *  un-touched fields as zero. Units follow SI (volts, amperes, Hz, seconds,
 *  percent). Integer fields are raw register values unless annotated.
 * --------------------------------------------------------------------------- */

struct ups_data {
    /* ---- Status block (driver->read_status populates) ---- */
    uint32_t status;              /* UPS_ST_* bits */
    uint16_t transfer_reason;     /* APC UPSStatusChangeCause_EN enum */
    uint32_t outlet_mog;          /* Main Outlet Group status bits */
    uint32_t outlet_sog0;         /* Switched Outlet Group 0 status bits */
    uint32_t outlet_sog1;         /* Switched Outlet Group 1 status bits */
    uint16_t sig_status;          /* UPS_SIG_* bits */
    uint16_t general_error;       /* UPS_GENERR_* bits */
    uint32_t power_system_error;  /* UPS_PWRERR_* bits */
    uint16_t bat_system_error;    /* UPS_BATERR_* bits */
    uint16_t bat_test_status;     /* APC ReplaceBatteryTestStatus_BF raw */
    uint16_t rt_cal_status;       /* APC RunTimeCalibrationStatus_BF raw */
    uint16_t bat_lifetime_status; /* APC Battery.LifeTimeStatus_BF raw */
    uint16_t ui_status;           /* APC UserInterfaceStatus_BF raw */

    /* ---- Dynamic block (driver->read_dynamic populates) ---- */
    uint32_t runtime_sec;         /* remaining runtime on battery, seconds */
    double   charge_pct;          /* battery state-of-charge, 0..100 */
    double   battery_voltage;     /* volts DC */
    double   load_pct;            /* output real power, 0..100 (may exceed on overload) */
    double   output_current;      /* amperes AC, RMS */
    double   output_voltage;      /* volts AC, RMS */
    double   output_frequency;    /* Hz */
    uint32_t output_energy_wh;    /* cumulative watt-hours at output */

    /* Bypass — SRT only; drivers without bypass leave as 0. */
    uint16_t bypass_status;       /* APC Bypass.InputStatus_BF raw */
    double   bypass_voltage;      /* volts AC */
    double   bypass_frequency;    /* Hz */

    /* Input. */
    uint16_t input_status;        /* APC Input.InputStatus_BF raw */
    double   input_voltage;       /* volts AC */

    /* Efficiency — read efficiency_reason first; `efficiency` is only valid
     * when reason == UPS_EFF_OK. See ups_eff_reason_t above. */
    double            efficiency;         /* percent, 0..100 when reason == OK */
    ups_eff_reason_t  efficiency_reason;  /* why efficiency is (or isn't) valid */

    /* Countdown timers — -1 means "no countdown active". */
    int16_t  timer_shutdown;      /* seconds until output off */
    int16_t  timer_start;         /* seconds until output on */
    int32_t  timer_reboot;        /* seconds until reboot cycle completes */
};

/* ---------------------------------------------------------------------------
 *  ups_inventory_t — one-shot identifying info cached at connect time
 * --------------------------------------------------------------------------- */

struct ups_inventory {
    char     model[33];           /* "SMT1500RM2UC", "Smart-UPS 1500", etc. */
    char     serial[17];          /* factory serial number */
    char     firmware[17];        /* firmware version string (e.g. "UPS 04.1") */
    uint16_t nominal_va;          /* nameplate VA rating */
    uint16_t nominal_watts;       /* nameplate watt rating */
    uint16_t sog_config;          /* SOG wiring / mode config, raw register value */
    /* Note: mutable settings (frequency tolerance, transfer thresholds,
     * outlet delays) do NOT belong here. Read them through the config
     * register API (ups_find_config_reg + ups_config_read) so the value
     * is always fresh rather than frozen at connect time. */
};

/* ---------------------------------------------------------------------------
 *  ups_t — live UPS context
 *
 *  Allocated by ups_connect(), released by ups_close(). Consumers normally
 *  use the accessor functions (ups_has_cap, ups_driver_name, etc.) rather
 *  than reading fields directly; the struct is exposed for contexts that
 *  need to inspect saved params for reconnect.
 * --------------------------------------------------------------------------- */

typedef struct ups_context {
    void               *transport;  /* opaque, owned by driver */
    const ups_driver_t *driver;

    /* Effective capability set — driver->caps optionally narrowed by
     * driver->resolve_caps at connect time. Always use ups_has_cap() to
     * query this; reading the field directly skips a NULL check. */
    uint32_t            caps;

    /* Effective config register set — a heap-allocated subset populated
     * from driver->config_regs by driver->resolve_config_regs if provided.
     * NULL when the driver has no resolver (callers fall back to
     * driver->config_regs). Freed by ups_close. Read via
     * ups_get_config_regs, which handles both cases. */
    ups_config_reg_t   *resolved_regs;
    size_t              resolved_regs_count;

    ups_inventory_t     inventory;      /* populated at connect time if possible */
    int                 has_inventory;  /* non-zero iff inventory fields are valid */

    /* Concurrency: serializes every driver callback. Acquired by the
     * registry wrappers (ups_read_*, ups_cmd_execute, ups_config_*).
     * Callers must NOT lock this themselves. */
    pthread_mutex_t     cmd_mutex;

    /* Connection health tracking. After 5 consecutive read failures the
     * registry tears down the transport; last_reconnect_attempt
     * rate-limits retries to roughly one every 2 seconds. */
    int                 consecutive_errors;
    time_t              last_reconnect_attempt;

    /* Saved params for reconnect. String members point into the
     * _device_buf / _serial_buf / _host_buf slots below so they survive
     * the caller's stack. */
    ups_conn_params_t   params;
    char                _device_buf[256];
    char                _serial_buf[64];
    char                _host_buf[256];
} ups_t;

/* ===========================================================================
 *  Connection lifecycle
 * =========================================================================== */

/* Connect to a UPS and auto-detect the appropriate driver.
 *
 * Iterates registered drivers whose conn_type matches params->type. For each
 * matching driver, calls connect() and then detect(). The first driver to
 * claim the UPS wins; on success *out receives a newly allocated ups_t that
 * the caller must release with ups_close().
 *
 * Return values:
 *   UPS_OK            — *out points at a live, detected UPS
 *   UPS_ERR_NO_DRIVER — no registered driver identified this UPS (either no
 *                       driver matched conn_type, or every matching driver's
 *                       detect() returned 0)
 *   UPS_ERR_IO        — every matching driver's connect() failed at the
 *                       transport layer (serial port couldn't open, USB
 *                       device not present, etc.), or allocation failed
 *
 * On any non-UPS_OK return, *out is set to NULL. */
int  ups_connect(const ups_conn_params_t *params, ups_t **out);

/* Release a ups_t from ups_connect(). Calls the driver's disconnect() and
 * frees the context. Safe to call with NULL. */
void ups_close(ups_t *ups);

/* ===========================================================================
 *  Basic queries (cheap, no transport I/O)
 * =========================================================================== */

/* Return non-zero if the UPS's effective capability set includes `cap`.
 * Reads from ups->caps, which may have been narrowed by the driver's
 * resolve_caps at connect time — so a `true` here means the feature will
 * actually work at runtime, not just that the driver struct declared it. */
int ups_has_cap(const ups_t *ups, ups_cap_t cap);

/* Name of the active driver ("srt", "smt", "backups_hid"). Stable across
 * the ups_t's lifetime. */
const char *ups_driver_name(const ups_t *ups);

/* Whether the underlying transport is currently open. Tracks the recovery
 * layer's ground truth: a single failed read does not flip this — only a
 * sustained failure that forces ups_handle_error to tear down the transport
 * (see MAX_CONSECUTIVE_ERRORS in ups.c). The monitor uses this to avoid
 * firing disconnect/reconnect events on transient flakes. */
int ups_is_connected(const ups_t *ups);

/* Power-path topology for this UPS. Calls the driver's get_topology
 * callback if set; otherwise returns the static topology field. */
ups_topology_t ups_topology(const ups_t *ups);

/* ===========================================================================
 *  Frequency tolerance settings
 *
 *  Only meaningful when ups_has_cap(ups, UPS_CAP_FREQ_TOLERANCE) is true.
 *  The getters return NULL with *count=0 when the capability is absent.
 * =========================================================================== */

const ups_freq_setting_t *ups_get_freq_settings(const ups_t *ups, size_t *count);
const ups_freq_setting_t *ups_find_freq_setting(const ups_t *ups, const char *name);
const ups_freq_setting_t *ups_find_freq_value  (const ups_t *ups, uint16_t value);

/* ===========================================================================
 *  Reads — populate caller-provided struct
 *
 *  All return UPS_OK on success, UPS_ERR_IO on driver failure,
 *  UPS_ERR_NOT_SUPPORTED if the active driver lacks the callback.
 *  Callers may share a single ups_data_t across status + dynamic reads.
 * =========================================================================== */

/* Populate the status fields of *data from the UPS. Also clears the
 * battery-related fields of *data when UPS_BATERR_DISCONNECTED is set. */
int ups_read_status   (ups_t *ups, ups_data_t *data);

/* Populate the dynamic measurement fields of *data. Also clears battery
 * fields when UPS_BATERR_DISCONNECTED is set on the existing data. */
int ups_read_dynamic  (ups_t *ups, ups_data_t *data);

/* Populate *inv with model / serial / firmware / ratings. Normally called
 * once at connect time; the result is cached on ups->inventory. */
int ups_read_inventory(ups_t *ups, ups_inventory_t *inv);

/* Read the input-voltage transfer thresholds in VOLTS.
 *
 * Drivers are required to return the values pre-scaled so every consumer
 * can treat the result as volts directly. The uint16_t type is legacy —
 * it reflects APC's native storage — but the *contract* is volts, not
 * raw register counts. */
int ups_read_thresholds(ups_t *ups, uint16_t *transfer_high, uint16_t *transfer_low);

/* ===========================================================================
 *  Commands — descriptor-driven operator actions
 *
 *  Drivers declare a static array of ups_cmd_desc_t; the API enumerates and
 *  dispatches them by name. All these functions are thread-safe.
 * =========================================================================== */

/* Get the command descriptor array from the active driver. Returns NULL
 * and sets *count=0 if the driver exposes no commands. */
const ups_cmd_desc_t *ups_get_commands(const ups_t *ups, size_t *count);

/* Find a command descriptor by `name`. Returns NULL if not found. */
const ups_cmd_desc_t *ups_find_command(const ups_t *ups, const char *name);

/* Find a command by flag (typically UPS_CMD_IS_SHUTDOWN or UPS_CMD_IS_MUTE).
 * Returns the first descriptor with `flags & flag` non-zero, or NULL. */
const ups_cmd_desc_t *ups_find_command_flag(const ups_t *ups, uint32_t flag);

/* Execute a command by name.
 *   is_off: TOGGLE commands use this to pick execute() (0) vs execute_off() (1);
 *           SIMPLE commands ignore it.
 *
 * The registry serializes against other driver calls and sleeps 200 ms
 * after the handler returns before releasing the mutex, so the UPS has
 * a quiet window to process the write before the next read.
 *
 * Returns UPS_OK, UPS_ERR_IO, or UPS_ERR_NOT_SUPPORTED (command not found
 * or TOGGLE off-handler missing). */
int ups_cmd_execute(ups_t *ups, const char *name, int is_off);

/* ===========================================================================
 *  Config register access
 *
 *  Drivers declare a static array of ups_config_reg_t describing readable/
 *  writable settings. Callers read by descriptor rather than raw address so
 *  the same API key works across drivers even when the underlying register
 *  numbers differ.
 * =========================================================================== */

const ups_config_reg_t *ups_get_config_regs(const ups_t *ups, size_t *count);
const ups_config_reg_t *ups_find_config_reg(const ups_t *ups, const char *name);

/* Read a config register.
 *   reg        : descriptor returned from ups_find_config_reg
 *   raw_value  : for scalar / bitfield types, receives the raw uint16.
 *                Ignored for UPS_CFG_STRING.
 *   str_buf    : for UPS_CFG_STRING, receives a NUL-terminated ASCII string
 *                (<= str_bufsz-1 characters). Ignored for other types.
 *
 * Returns UPS_OK, UPS_ERR_IO, or UPS_ERR_NOT_SUPPORTED. */
int ups_config_read(ups_t *ups, const ups_config_reg_t *reg,
                    uint16_t *raw_value, char *str_buf, size_t str_bufsz);

/* Write a config register. The registry enforces a 100 ms inter-write delay
 * between successive config_writes against the same UPS and always reads
 * the register back after writing (the driver may verify internally too).
 *
 * Returns UPS_ERR_NOT_SUPPORTED if reg->writable is 0 or the driver has no
 * config_write handler; UPS_ERR_IO on write failure. */
int ups_config_write(ups_t *ups, const ups_config_reg_t *reg, uint16_t value);

#endif
