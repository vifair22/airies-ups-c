# UPS Driver API

Reference for writing and maintaining UPS drivers in **airies-ups-c**.

This document describes the contract a UPS driver implements and the public
surface every other subsystem (daemon, API routes, monitor thread, shutdown
orchestrator, alert engine, weather) sees. Code is source-of-truth: the
canonical declarations live in [`src/ups/ups_driver.h`](src/ups/ups_driver.h)
(driver-facing) and [`src/ups/ups.h`](src/ups/ups.h) (consumer-facing). This
doc exists so you can get oriented without re-reading those headers every
session.

---

## Overview

Drivers are `static const ups_driver_t` instances linked at compile time and 
listed in the registry (`ups_drivers[]` in `src/ups/ups.c`). At runtime, the
daemon calls `ups_connect(&params, &ups)`, which iterates registered drivers
whose `conn_type` matches the caller's transport, asks each one to probe the
bus, and picks the first one whose `detect()` returns 1.

```mermaid
flowchart TD
    A[daemon config] -->|conn_type, device, etc.| B[ups_connect]
    B --> C{for each driver<br/>matching conn_type}
    C -->|drv.connect| D[open transport]
    D -->|NULL| C
    D -->|transport*| E[drv.detect]
    E -->|returns 0| F[drv.disconnect]
    F --> C
    E -->|returns 1| G[drv.resolve_caps<br/>narrow caps]
    G --> H[ups_read_inventory<br/>cache model/serial/fw]
    H --> I[return ups_t*]
    C -->|exhausted| J[UPS_ERR_NO_DRIVER or UPS_ERR_IO]
```

Once a driver wins the registry race:

1. The **monitor thread** begins polling `read_status` + `read_dynamic` on
   a fixed cadence (default 2 s), writing telemetry snapshots to SQLite
   every telemetry interval (default 30 s).
2. **API routes** call `ups_cmd_execute` and `ups_config_read` / `_write`
   as operator actions arrive.
3. The **alert engine** and **shutdown orchestrator** hook the monitor's
   per-poll callback to evaluate thresholds.
4. On SIGTERM/SIGINT, the daemon calls `ups_close(ups)` which invokes
   `drv.disconnect(transport)`.

The driver never knows about any of the consumers — it just implements the
vtable and returns when called.

---

## The driver struct

`ups_driver_t` is the vtable. Look at
[`src/ups/ups_driver.h`](src/ups/ups_driver.h) for field-by-field doc
comments; this section is a high-altitude reference.

| Group | Fields | Notes |
|-------|--------|-------|
| Identity | `name`, `conn_type`, `topology` | Stable ID + transport + default power-path. |
| Lifecycle | `connect(params)`, `disconnect(transport)` | Driver owns the `void *transport` handle between these two calls. |
| Identification | `detect(transport)`, `get_topology(transport)` (optional) | `detect` returns 1 iff this driver claims the device. `get_topology` overrides the static field per-connection. |
| Capabilities | `caps`, `resolve_caps(transport, default_caps)` (optional) | `caps` is the *maximum* set; `resolve_caps` narrows to what actually resolved on the live device. See [Capabilities](#capabilities). |
| Reads | `read_status`, `read_dynamic`, `read_inventory`, `read_thresholds` | See [Data structs](#data-structs). All return 0 on success, -1 on I/O failure. |
| Commands | `commands[]`, `commands_count` | Descriptor-driven. See [Commands](#commands). |
| Config | `config_regs[]`, `config_regs_count`, `resolve_config_regs` (optional), `config_read`, `config_write` | Optional spec-driven setting I/O. See [Config registers](#config-registers). |
| Freq tolerance | `freq_settings[]`, `freq_settings_count` | Only meaningful when `UPS_CAP_FREQ_TOLERANCE` is claimed. |

Drivers that don't need a particular feature leave the corresponding pointer
/ count as NULL / 0. Designated initializers make this cheap; see
`ups_srt.c` or `ups_backups_hid.c` for real examples.

---

## Lifecycle

### Connect (once per boot)

```mermaid
sequenceDiagram
    participant D as daemon/main.c
    participant R as ups_connect (ups.c)
    participant Drv as driver
    participant Tx as transport layer

    D->>R: ups_connect(&params, &out)
    loop each driver matching conn_type
        R->>Drv: connect(params)
        Drv->>Tx: open tty / hidraw / etc.
        Tx-->>Drv: handle or NULL
        Drv-->>R: transport* or NULL
        alt transport* valid
            R->>Drv: detect(transport)
            Drv-->>R: 1 (match) or 0 (not me)
            alt match
                R->>Drv: resolve_caps (if present)
                R->>Drv: read_inventory
                R-->>D: UPS_OK + ups_t*
            else mismatch
                R->>Drv: disconnect(transport)
            end
        end
    end
    R-->>D: UPS_ERR_NO_DRIVER | UPS_ERR_IO
```

### Steady state (per poll)

```mermaid
sequenceDiagram
    participant M as monitor thread
    participant R as ups_read_* (ups.c)
    participant L as cmd_mutex
    participant Drv as driver.read_*

    loop every poll_sec
        M->>R: ups_read_status + ups_read_dynamic
        R->>L: lock
        R->>Drv: read_status(transport, data)
        Drv-->>R: 0 / -1
        R->>Drv: read_dynamic(transport, data)
        Drv-->>R: 0 / -1
        R->>L: unlock
        R-->>M: rc
        M->>M: alerts, shutdown triggers, event emit
    end
```

### Command dispatch

API thread takes the same `cmd_mutex`, runs the driver's `execute` (or
`execute_off` for the OFF side of a toggle), sleeps 200 ms while still
holding the lock, then releases. That quiet window is part of the contract
— drivers and callers both depend on it.

### Reconnect

The registry tracks `consecutive_errors` on the `ups_t`. After five failed
reads in a row, it tears down the transport (`driver->disconnect`) and
leaves `transport = NULL`. The next `ups_read_*` call triggers
`ups_try_reconnect`, which — at most once per 2 s — walks the same
post-detect steps as the initial connect:

```mermaid
sequenceDiagram
    participant R as ups_try_reconnect
    participant Drv as driver

    R->>Drv: connect(ups->params)
    Drv-->>R: transport* or NULL
    alt transport*
        R->>Drv: detect(transport)
        alt detect == 1
            R->>Drv: resolve_caps
            R->>Drv: resolve_config_regs
            R->>Drv: read_inventory
            R->>R: install transport, clear errors
        else different device
            R->>Drv: disconnect(transport)
        end
    end
```

Driver author implication: **`connect()` must be replayable**. The registry
will call it over and over while the physical layer is down, and a
successful replay triggers another round of `detect` / `resolve_caps` /
`resolve_config_regs` / `read_inventory`. Any state the driver caches
during `connect` (e.g., Back-UPS HID's resolved field pointers) must be
recomputed each time — drivers generally achieve this by allocating a
fresh transport struct on every `connect`.

---

## Transports

`void *transport` is opaque to the app. Each driver casts it to whatever
its library needs.

| Driver | `conn_type` | Transport handle | Lifetime |
|--------|-------------|-------------------|----------|
| `ups_srt`, `ups_smt` | `UPS_CONN_SERIAL` | `modbus_t *` (libmodbus) | `modbus_new_rtu` in `connect`, `modbus_free` in `disconnect` |
| `ups_backups_hid` | `UPS_CONN_USB` | `hid_transport_t *` (in-tree, see `ups_backups_hid.c`) | hidraw fd + parsed descriptor + resolved field pointers |
| *(future)* | `UPS_CONN_TCP` | TBD | Placeholder; no driver yet |
| *(future)* | `UPS_CONN_SNMP` | TBD | Placeholder; no driver yet |

### Modbus RTU (serial)

Drivers use libmodbus directly. Typical `connect` body:

```c
modbus_t *ctx = modbus_new_rtu(params->serial.device,
                               params->serial.baud, 'N', 8, 1);
if (!ctx) return NULL;
modbus_set_slave(ctx, params->serial.slave_id);
modbus_set_response_timeout(ctx, 5, 0);
if (modbus_connect(ctx) < 0) { modbus_free(ctx); return NULL; }
return ctx;
```

Reads use `modbus_read_registers(ctx, addr, count, regs)` (function 0x03,
Read Holding Registers). Writes use `modbus_write_register` /
`modbus_write_registers`.

APC quirk: some legacy SMT firmwares (pre-09.0) accept both function 3
(holding) and function 4 (input registers) for the same data. The unified
map (AN-176 and 990-9840B) is function-3-only. Stick to function 3 unless
you have a reason not to.

### USB HID

The transport handle is a `hid_transport_t` defined in `ups_backups_hid.c`.
The important bits:

- `fd`: open `/dev/hidrawN`
- `map`: parsed HID report descriptor (`hid_report_map_t`)
- `f`: resolved field pointers keyed by HID usage path (see
  `resolve_fields` in the driver)

Field reads use `hid_field_read_raw` / `hid_field_read_scaled` from
`hid_parser.h`. Writes use `hid_set_feature`. A field may be NULL if the
specific device didn't declare that usage in its descriptor — this is the
main reason `backups_resolve_caps()` exists (see [Capabilities](#capabilities)).

---

## Data structs

See `ups_data_t` and `ups_inventory_t` in
[`src/ups/ups.h`](src/ups/ups.h) for per-field docs with units and
sentinels.

### `ups_data_t` — per-poll snapshot

Populated by `read_status` + `read_dynamic`. SI units throughout (volts,
amperes, Hz, seconds, percent). A few fields have meaningful sentinels:

- `timer_shutdown`, `timer_start`: `-1` means "no countdown active"
- `efficiency`: valid only when `efficiency_reason == UPS_EFF_OK`. See
  [Efficiency encoding](#efficiency-encoding).
- Battery fields (`charge_pct`, `battery_voltage`, `runtime_sec`) are
  force-zeroed by the registry if `bat_system_error & UPS_BATERR_DISCONNECTED`
  is set.

Un-populated fields stay zero. Drivers should only write fields they
actually read.

### `ups_inventory_t` — one-shot identity

Cached on `ups->inventory` at connect time via `ups_read_inventory` and
refreshed on reconnect. Holds **identity fields only** (model, serial,
firmware, nameplate ratings, SOG wiring config). Consumers may use
`ups->has_inventory` to gate display.

**Not here:** mutable settings like frequency tolerance, transfer
thresholds, or outlet delays. Those change at runtime and must be read
fresh via the config register API (`ups_find_config_reg` +
`ups_config_read`). Caching them in inventory would leave the UI showing
stale values after a write.

### `ups_read_thresholds` contract

Returns `transfer_high` and `transfer_low` in **volts**, not raw register
counts. Drivers must pre-scale; the `uint16_t` parameter type is legacy
(reflects APC's native storage) but the semantic is volts throughout.
Consumers can treat the result as a voltage directly.

### Efficiency encoding

APC reports efficiency as a signed integer (raw/128 = percent) with
negative values repurposed as reason codes (load too low, on battery, etc.).
Rather than leak that encoding everywhere, drivers split the raw into two
fields:

- `efficiency_reason` (enum `ups_eff_reason_t`): why the reading is
  available or not
- `efficiency` (double): valid percent only when
  `efficiency_reason == UPS_EFF_OK`; otherwise 0

Consumers that render or compare efficiency must check `reason == UPS_EFF_OK`
first. The shutdown orchestrator does this by returning NaN for the
comparator when reason isn't OK, so numeric triggers silently skip.

---

## Capabilities

Capability flags gate feature surface — UI buttons, API responses, the
shutdown orchestrator's target search. A capability is *honest* when
claiming it guarantees the feature will actually work against the live
device.

Each driver declares a **maximum** set in `driver->caps`. At connect
time, the registry calls `driver->resolve_caps(transport, driver->caps)` if
present, letting the driver clear bits for features that didn't resolve
against this specific device. The returned mask is stored on `ups_t->caps`,
and every `ups_has_cap()` query reads from there.

Example (Back-UPS HID):

```c
static uint32_t backups_resolve_caps(void *transport, uint32_t default_caps)
{
    hid_transport_t *t = transport;
    uint32_t caps = default_caps;

    if (!t->f.delay_before_shutdown) caps &= ~UPS_CAP_SHUTDOWN;
    if (!t->f.test)                  caps &= ~UPS_CAP_BATTERY_TEST;
    if (!t->f.module_reset)          caps &= ~UPS_CAP_CLEAR_FAULTS;
    if (!t->f.alarm_control) {
        caps &= ~UPS_CAP_MUTE;
        caps &= ~UPS_CAP_BEEP;
    }
    return caps;
}
```

If a driver doesn't need to narrow (e.g., the SRT always supports
everything it advertises), just leave `resolve_caps = NULL`.

### Claimed capability ↔ obligation

| Capability | Obligation |
|------------|------------|
| `UPS_CAP_SHUTDOWN` | A command in `commands[]` with `UPS_CMD_IS_SHUTDOWN` set in `flags`. |
| `UPS_CAP_BATTERY_TEST` | A command named "battery_test" (or equivalent). |
| `UPS_CAP_RUNTIME_CAL` | Commands for start/stop of runtime calibration. |
| `UPS_CAP_CLEAR_FAULTS` | A command that resets latched fault indicators. |
| `UPS_CAP_MUTE` | A command with `UPS_CMD_IS_MUTE` set. |
| `UPS_CAP_BEEP` | A short-beep-test command. |
| `UPS_CAP_BYPASS` | A toggle command (see [Commands](#commands)) whose `status_bit` reflects `UPS_ST_BYPASS`. |
| `UPS_CAP_FREQ_TOLERANCE` | `freq_settings[]` populated with at least one option. |
| `UPS_CAP_HE_MODE` | `ups_data.status` reflects `UPS_ST_HE_MODE` correctly. |

---

## Convenience tables

### `config_regs[]`

Descriptor-driven, driver-supplied table of readable/writable UPS settings.
The daemon exposes them via `/config/<name>` endpoints without knowing the
register layout of each family.

Descriptor fields include: API `name`, `display_name`, `unit`, `group`,
`reg_addr` (driver-interpreted), `reg_count`, `type` (`SCALAR`, `BITFIELD`,
`STRING`), `scale` divisor, `writable` flag, and type-specific metadata
(min/max for scalar, options array for bitfield, `max_chars` for string).

The driver's `config_read` / `config_write` callbacks translate the
descriptor into transport-level I/O. Back-UPS HID uses `reg_addr` as an
opaque field-lookup key; Modbus drivers use it as an absolute register
address.

#### Narrowing at connect time: `resolve_config_regs`

Analogous to `resolve_caps`. The driver's static `config_regs[]` is the
maximum set; `resolve_config_regs(transport, default_regs, default_count,
out)` lets the driver narrow to descriptors whose backing
register/HID-field actually resolved on the live device. The registry
allocates `out` with capacity `default_count`, the driver writes the
subset as struct copies, and the result is cached on `ups_t->resolved_regs`.
`ups_get_config_regs` returns the narrowed set when present and the full
static array otherwise.

Back-UPS HID uses this to drop descriptors whose HID field didn't resolve
on the current device:

```c
static size_t backups_resolve_config_regs(
    void *transport, const ups_config_reg_t *default_regs,
    size_t default_count, ups_config_reg_t *out)
{
    hid_transport_t *t = transport;
    (void)default_count;
    size_t n = 0;
    #define KEEP_IF(idx, f) do { if ((f)) out[n++] = default_regs[(idx)]; } while (0)
    KEEP_IF(CFG_TRANSFER_LOW,  t->f.transfer_low);
    // ...one line per descriptor index
    #undef KEEP_IF
    return n;
}
```

Drivers that don't need narrowing (SRT, currently) leave
`resolve_config_regs = NULL` and consumers see the static array. SMT uses
this to drop SOG descriptors for outlet groups that aren't physically
present on the SKU.

### `freq_settings[]`

Enumerates valid frequency-tolerance choices for the UPS. Only consumed
when `UPS_CAP_FREQ_TOLERANCE` is in the effective caps.

---

## Commands

Drivers declare operator actions in `commands[]`, each a `ups_cmd_desc_t`:

- `name` (API dispatch key), `display_name`, `description`, `group`
- `confirm_title` / `confirm_body` — optional modal text
- `type`: `UPS_CMD_SIMPLE` (one handler) or `UPS_CMD_TOGGLE` (on + off)
- `variant`: visual weight (`DEFAULT`, `WARN`, `DANGER`)
- `flags`: feature bits (`UPS_CMD_IS_SHUTDOWN`, `UPS_CMD_IS_MUTE`)
- `status_bit`: for toggles, which `ups_data.status` bit reflects the
  "on" state
- `execute(transport)`, `execute_off(transport)` handlers

Handlers run under `cmd_mutex` with a guaranteed 200 ms quiet window after
return. Return 0 on success, -1 on error.

---

## Config registers

Same table as [Convenience tables](#convenience-tables) — repeated here
because when implementing a new driver, you'll touch this twice: once
to define the static table, and again to implement the driver's
`config_read` / `config_write` callbacks. See `ups_srt.c:srt_config_regs`
and `srt_config_read` / `srt_config_write` for the Modbus example, and
`ups_backups_hid.c:backups_config_regs` for the HID-field-mapped example.

---

## Error conventions

| Code | Meaning |
|------|---------|
| `UPS_OK` (0) | Success |
| `UPS_ERR_IO` (-1) | I/O failure: transport down, CRC error, driver returned -1 |
| `UPS_ERR_NO_DRIVER` (-2) | No registered driver identified the UPS |
| `UPS_ERR_NOT_SUPPORTED` (-3) | The active driver lacks the requested callback |

All negative, so `if (rc < 0)` catches every failure. Driver-level
callbacks return 0 / -1; the registry translates to the public-API codes.

There is no errno-style thread-local error message buffer; callers log
failures at the call site. The registry tracks
`ups->consecutive_errors` and tears down the transport after 5 consecutive
read failures, rate-limiting reconnect attempts to one every ~2 seconds.

---

## Threading

**The registry serializes all driver access via `ups->cmd_mutex`.** Every
`ups_read_*`, `ups_cmd_execute`, and `ups_config_*` acquires that mutex,
calls the driver, and releases. Consequences:

- Driver callbacks run single-threaded as far as `transport` is concerned.
- Drivers MUST NOT spawn threads that touch `transport`. If the driver
  needs internal asynchrony, it must serialize itself.
- Concurrent callers (monitor thread + API route + shutdown orchestrator)
  will stall each other if one is blocked in a slow I/O. In practice
  Modbus reads finish in under ~100 ms, so contention is invisible, but
  it's a real cost when one read hangs waiting for timeout.
- After every successful command execute or config_write, the registry
  sleeps 200 ms *before* releasing the mutex. Drivers can rely on that
  quiet window for UPS firmware to process the write before the next
  read races in.

The inventory cache (`ups->inventory`) is written exactly once at connect
time and read without a lock thereafter; `has_inventory` is the guard.

---

## Testing

Each test binary links against `src/ups/ups.c`, which references
`extern const ups_driver_t ups_driver_srt / _smt / _backups_hid`. To
satisfy the linker without pulling in real driver code and the libraries
they need (libmodbus, hidraw open paths), `tests/test_stubs.c` declares
each driver as a minimal empty `ups_driver_t`:

```c
const ups_driver_t ups_driver_srt = {
    .name = "srt_stub", .conn_type = UPS_CONN_SERIAL,
    .topology = UPS_TOPO_ONLINE_DOUBLE, .caps = 0,
};
```

Designated initializers zero-init all other fields, including function
pointers — which is safe because the registry NULL-checks every callback.

For driver-specific logic (e.g., string formatting, register decoding),
tests invoke the helpers directly without going through the registry.
See `tests/test_ups_strings.c` for the pattern.

---

## New-driver checklist

Adding a new driver (e.g., an SNMP or Modbus TCP driver):

1. **Files** — create `src/ups/ups_<family>.c`. Header-in-c file; no
   separate `.h` unless the driver exposes helpers to other translation
   units (rare).
2. **Static driver struct** — `static const ups_driver_t ups_driver_<family>`
   at the bottom of the file, with every required callback pointing at a
   file-scope static function.
3. **Connect / disconnect** — own the `void *transport` lifetime. Must be
   re-entrant-safe; the registry calls speculatively across drivers.
4. **Detect** — cheap probe (one or two reads); return 1 if you recognize
   the device. Keep this small; it runs on every connect candidate.
5. **Resolve caps** (if needed) — narrow the maximum-cap mask based on
   what resolved against the live device. See Back-UPS example above.
6. **Reads** — `read_status`, `read_dynamic`, `read_inventory`,
   `read_thresholds`. Populate the relevant `ups_data_t` /
   `ups_inventory_t` fields and leave the rest zeroed. Remember
   `read_thresholds` returns **volts**, not raw register counts, and
   `ups_inventory_t` is identity-only (no mutable settings).
7. **Commands & config regs** — static tables + handler functions. Match
   each claimed capability to a command per the table in
   [Capabilities](#capabilities). If some descriptors are only present
   on a subset of devices in your family, implement
   `resolve_config_regs` to narrow at connect time (parallel to
   `resolve_caps`).
8. **Register in `src/ups/ups.c`** — add `extern const ups_driver_t
   ups_driver_<family>;` and an entry in `ups_drivers[]`. Order matters:
   put specific drivers before permissive ones to avoid misclassification.
9. **Stub in `tests/test_stubs.c`** — add the empty `const
   ups_driver_t ups_driver_<family>` stub so tests link.
10. **Makefile** — source files are discovered by pattern; if your driver
    fits `src/ups/*.c`, it builds automatically.
11. **Build & test** — `make clean && make debug && make test`. All
    warnings are errors; the project uses strict flags.
12. **Smoke-test against real hardware** — run `./airies-upsd` against
    the actual UPS, watch `stdout.log`, verify detect and steady-state
    reads. The DEPLOY.md describes the local dev path.

---

## Known rough edges

Not blocking, but worth fixing when someone has the cycles:

- **`BACKUPS_CFG_COUNT` is hardcoded** (`ups_backups_hid.c`). Keep in
  sync with the array size manually; no compile-time assertion.
- **`backups_hid` output-voltage inference** is derived from
  `status & ON_BATTERY ? nominal : input_voltage`. Accurate for standby
  UPS but not perfect during transitional states.
- **Modbus TCP / SNMP are placeholder enums**. `ups_connect` will return
  `UPS_ERR_NO_DRIVER` for either until a driver lands.

---

## References

- [`src/ups/ups_driver.h`](src/ups/ups_driver.h) — driver vtable, detailed
  field-by-field comments
- [`src/ups/ups.h`](src/ups/ups.h) — consumer-facing API, status bits,
  error codes, data structs
- [`src/ups/ups.c`](src/ups/ups.c) — registry, connect, mutex-wrapped
  callback dispatch
- [`docs/apc/990-9840_modbus_register_map.pdf`](docs/apc/990-9840_modbus_register_map.pdf)
  — unified SMT/SMX/SURTD/SRT register map (APC)
- [`docs/apc/AN176_modbus_implementation.pdf`](docs/apc/AN176_modbus_implementation.pdf)
  — protocol-level spec (framing, BPI encoding, register-block layout)
