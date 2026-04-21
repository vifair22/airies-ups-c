# airies-ups — UPS Management Platform

## Vision

A self-contained UPS management system that replaces APC PowerChute with something that actually works. Full monitoring, control, configuration, weather-aware HE mode management, and orchestrated shutdown — all through a modern web UI and CLI, backed by direct Modbus RTU communication.

One Pi per UPS. One daemon per Pi. No NUT, no PowerChute, no cloud dependency.

---

## Architecture

### Binaries

| Binary | Purpose |
|--------|---------|
| `airies-upsd` | Daemon. Owns serial port, UPS communication, monitoring, alerts, shutdown orchestration, weather polling, web UI (HTTP API + static frontend), SQLite database, Pushover notifications. |
| `airies-ups` | CLI. Talks to daemon via unix socket REST API. Never touches serial or DB directly. Used for scripting, quick commands, and headless management. |

### Data Flow

```
                    ┌─────────────────────────────────┐
                    │         airies-upsd              │
                    │                                  │
  /dev/ttyUSB0 ◄──►│  UPS Driver (SRT/SMT)            │
                    │    ├── Monitor (2s poll)          │
                    │    ├── Alert Engine               │
                    │    ├── Shutdown Orchestrator       │
                    │    └── Config Register R/W        │
                    │                                  │
  NWS API ◄────────│  Weather Subsystem (5m poll)      │
                    │                                  │
  SQLite DB ◄──────│  Storage                          │
                    │    ├── Telemetry (time-series)    │
                    │    ├── Event Journal              │
                    │    ├── App Config                 │
                    │    ├── UPS Config Snapshots       │
                    │    ├── Shutdown Targets/Groups    │
                    │    └── Weather Config             │
                    │                                  │
  TCP :8080 ◄──────│  HTTP API + Static Frontend       │
  Unix socket ◄────│  CLI API (same REST routes)       │
                    └─────────────────────────────────┘
                          ▲                ▲
                          │                │
                    airies-ups (CLI)   Browser (React)
```

### API

Single REST-style JSON API served over both TCP (for web UI / remote access) and unix socket (for CLI). Same route handlers, same request/response format.

Auth: HTTP Basic or token-based. Admin user + password stored in DB (hashed). CLI authenticates once and caches a token. Web UI uses standard session/token flow.

### Database (SQLite)

Single `airies-ups.db` file. Daemon owns it exclusively.

**Tables:**

| Table | Purpose |
|-------|---------|
| `config` | Key-value app configuration (serial device, baud, slave ID, web port, Pushover creds, poll intervals) |
| `telemetry` | Time-series UPS measurements (timestamp, voltage, load, charge, runtime, frequency, temp, efficiency, input voltage) |
| `events` | Event journal (timestamp, severity, category, title, message). Replaces Pushover as the canonical log — Pushover becomes a notification channel fed from this. |
| `ups_config` | UPS register config snapshots (register name, value, last read timestamp). Lets you diff config over time. |
| `shutdown_targets` | Shutdown host definitions (name, method, connection string, command, timeout, group, order within group) |
| `shutdown_groups` | Group definitions (name, order, parallel flag). Groups execute sequentially. Hosts within a group execute in parallel. Final implicit group: UPS shutdown command + self-shutdown. |
| `weather_config` | Weather subsystem config (lat/lon, NWS zones, alert types, wind threshold, forecast keywords, poll interval) |

### First-Run Bootstrap

If the DB doesn't exist or has no config:
1. Daemon starts, binds HTTP on configured port (default 8080, overridable via CLI flag)
2. No UPS connection attempted yet
3. Web UI shows setup wizard:
   - Set admin password
   - Configure serial port (scan available ports, test connection)
   - Auto-detect UPS model + driver
   - Show detected UPS info (model, serial, firmware, ratings)
   - Optional: configure Pushover, weather, shutdown targets
4. On completion, daemon connects to UPS and begins monitoring

CLI flag overrides for headless bootstrap: `airies-upsd --port 8080 --setup-serial /dev/ttyUSB0`

---

## Driver Architecture (existing, extended)

The abstraction + driver layer we just built carries forward. Extensions:

### Config Register Descriptors

Each driver provides a table of config register descriptors:

```c
typedef struct {
    const char *name;           /* "transfer_high" */
    const char *display_name;   /* "Input Transfer High Voltage" */
    const char *unit;           /* "V", "s", "%", NULL */
    uint16_t    reg_addr;
    uint8_t     reg_count;      /* 1 for uint16, 2 for uint32, N for string */
    uint8_t     type;           /* UPS_CFG_SCALAR, UPS_CFG_BITFIELD, UPS_CFG_STRING */
    uint16_t    scale;          /* divisor (1 = raw) */
    int         writable;
    /* Type-specific metadata */
    union {
        struct { int32_t min; int32_t max; } scalar;
        struct { const ups_bitfield_opt_t *opts; size_t count; } bitfield;
        struct { size_t max_chars; } string;
    };
} ups_config_reg_t;
```

Write path enforces 100ms inter-write delay. Read-back after write for verification.

### Telemetry Descriptor

Drivers also describe which telemetry fields they produce, so the storage and API layers know what's available without hardcoding field names.

---

## Subsystems

### Monitor

- Polls UPS status + dynamic blocks every 2s (configurable)
- Feeds telemetry to DB (downsampled — e.g., write every 30s unless a value changes significantly)
- Feeds state changes to event journal with granular per-bit transition events
- Triggers alert engine on state transitions
- Triggers shutdown orchestrator on shutdown condition

### Alert Engine

Current hysteresis-based system, extended:
- Alert definitions stored in DB (threshold, deadband, severity)
- Default alert set created on first run
- Configurable via web UI
- Fires events to journal + Pushover (filtered by configurable severity threshold)

### Weather Subsystem

Port of `airies-ups-weather` into C:
- NWS alert polling (by zone)
- NWS hourly forecast polling (by gridpoint)
- Wind speed threshold check
- Severe forecast keyword matching
- HE inhibit/restore logic with source tracking
- Config stored in DB, manageable via web UI
- Uses libcurl (already a dependency for Pushover)

### Shutdown Orchestrator

Replaces hardcoded SSH shutdown logic:
- Targets defined in DB: name, connection method (SSH key, SSH password, custom command), command, timeout
- Groups defined in DB: name, execution order, parallel flag
- Execution: groups run sequentially, targets within a group run in parallel (fork model)
- Each target: execute command, wait for host to go offline (ping check), timeout
- Final phase (always last): send UPS shutdown command (configurable delay), then shutdown self
- Dry-run mode for testing
- Status reporting during execution (per-target progress via API)
- Triggered automatically by shutdown condition or manually via command

### Web UI

React + Tailwind, built on dev machine, static bundle served by daemon.

**Pages:**

| Page | Content |
|------|---------|
| Dashboard | Topology-aware power flow: summary bar (model/serial/rating/status), 4 overview cards (battery, load, output, input with voltage bar), 3 cascading planes (Utility, UPS, Output) with color-coded health states |
| Telemetry | Historical charts (selectable time range, selectable metrics). Voltage, load, charge, runtime, temperature over time. |
| Events | Filterable event journal. Toggle filter chips for severity (info/warning/error/critical) and category (power/mode/fault/test/alert/system/weather/shutdown). |
| Commands | Grouped by operational use: Power Control (bypass with state-aware toggle + modal, shutdown with dry-run), Diagnostics (battery test, runtime cal, beep test with stop), Alarm & Fault Management (mute/unmute, clear faults). All with confirmation modals and toast notifications. |
| Config: UPS | Read/write UPS config registers. Grouped by category (transfer voltages, outlet delays, load shed, battery test interval, sensitivity). Shows current vs last-known values. |
| Config: App | App settings: serial port, poll intervals, Pushover config, push notification severity, bypass voltage thresholds, admin password change. |
| Config: Shutdown | Shutdown target and group management. Add/edit/remove targets, organize into groups, set execution order. Test individual targets. |
| Config: Weather | Weather subsystem config: location, NWS zones, alert types, wind threshold, forecast keywords, enable/disable. Current weather status. |
| Setup | First-run wizard (also accessible for reconfiguration). |

### CLI

Mirrors the API surface:

```
airies-ups status                          # live status dump
airies-ups events [--limit N] [--since T]  # recent events
airies-ups config dump                     # all UPS config registers
airies-ups config get <name>               # single register
airies-ups config set <name> <value>       # write register
airies-ups cmd shutdown [--dry-run]        # trigger shutdown workflow
airies-ups cmd battery-test                # start battery test
airies-ups cmd runtime-cal                 # start runtime calibration
airies-ups cmd bypass on|off               # toggle bypass
airies-ups cmd freq <setting>              # set frequency tolerance
airies-ups cmd mute|unmute                 # alarm mute
airies-ups cmd beep                        # beeper test
airies-ups cmd clear-faults                # clear fault register
airies-ups weather status                  # current weather assessment
airies-ups shutdown test <target>          # test single shutdown target
airies-ups app config get <key>            # app config
airies-ups app config set <key> <value>    # app config
```

---

## Implementation Phases

### Phase 0: c-utils Library -- COMPLETE

Build the shared C application framework (`~/git/home/c-utils`). Static library (`libc-utils.a`) with headers. This is a standalone project used by airies-ups and future C apps.

**Subsystems:**
- [x] **DB** — SQLite wrapper, thread-safe access, WAL mode
- [x] **Migrations** — numbered SQL files, SHA256 checksum tracking, savepoint-per-migration rollback, two-tier (lib + app)
- [x] **Config** — two-tier: core bootstrap (DB path, HTTP port, log level) via CLI flags/defaults + app-level config in DB with registration API (name, type, default, description), mutable at runtime
- [x] **Logging** — colored console + async SQLite persistence, log levels (debug/info/warn/error), auto-cleanup (configurable retention)
- [x] **Pushover** — DB-persisted notification queue, retry with exponential backoff, message splitting
- [x] **AppGuard** — lifecycle manager: ordered init (config → DB → migrations → logging → push), signal handlers, graceful shutdown in reverse order, clean self-restart via `appguard_restart()`
- [x] **Error loop detector** — consecutive error tracking with normalization, threshold + cooldown + callback
- [x] Makefile (builds `libc-utils.a`), tests (cmocka), static analysis

### Phase 1: Foundation -- COMPLETE

Restructure airies-ups for the new architecture. No new features — everything that works today keeps working.

- [x] Integrate c-utils (link `libc-utils.a`, replace `log.h` and `pushover.h` with c-utils subsystems)
- [x] Split into daemon (`airies-upsd`) + CLI (`airies-ups`) binaries
- [x] Migrate app config from INI file to c-utils DB config (register keys at init, import INI on first run)
- [x] Implement HTTP server in daemon (libmicrohttpd)
- [x] Implement REST API layer (JSON request/response via cJSON)
- [x] Serve API over both TCP (web UI) and unix socket (CLI)
- [x] Route CLI through API (CLI is pure API client, no direct Modbus)
- [x] Auth system (admin user/password, hashed in DB, token-based sessions)
- [x] Makefile restructure for two binaries + c-utils linkage + frontend build integration

### Phase 2: Data Layer -- COMPLETE

Build the storage and retrieval infrastructure.

- [x] Telemetry recording (monitor loop writes to SQLite, configurable sample interval)
- [x] Telemetry downsampling/retention (keep full resolution for 24h, downsample older data, configurable retention)
- [x] Event journal (replace ad-hoc log_msg + Pushover with structured events)
- [x] Pushover becomes a subscriber to the event journal (fires on configurable severity levels via `push.min_severity`)
- [x] Config register descriptors in SRT driver (SMT partial — needs hardware testing)
- [x] UPS config snapshot read/store
- [x] Config register read/write API endpoints

### Phase 3: Shutdown Orchestration -- COMPLETE

Replace hardcoded shutdown logic with the configurable system.

- [x] DB schema for targets and groups
- [x] Target execution engine (SSH key, SSH password, custom command)
- [x] Group execution engine (sequential groups, parallel within group)
- [x] Final phase: UPS shutdown command + self-shutdown
- [x] Dry-run mode
- [x] Per-target status reporting via API
- [x] API endpoints for CRUD on targets and groups
- [x] Migration: import existing config.ini shutdown targets into DB on first run

### Phase 4: Weather Integration -- COMPLETE

Port weather monitor from Python to C, integrated into daemon.

- [x] NWS API client (libcurl, JSON parsing via cJSON)
- [x] Alert zone polling
- [x] Forecast/wind polling with gridpoint resolution
- [x] HE inhibit/restore logic (port existing state machine)
- [x] Weather config in DB
- [x] Weather status + history in event journal
- [x] API endpoints for weather status and config

### Phase 5: Web Frontend -- COMPLETE (except setup wizard)

React + Tailwind SPA, built on dev machine, static bundle served by daemon.

- [x] Project setup (Vite + React + Tailwind)
- [x] Auth flow (login page, token management)
- [x] Dashboard page — topology-aware (SRT double-conversion / SMT line-interactive AVR)
      - Summary bar: model, serial, rating, driver, status dot, HE inhibit badge
      - 4 overview cards: Battery (charge/runtime/voltage), Load (% / watts / amps / VA), Output (voltage/frequency/outlet groups filtered by sog_config), Input (voltage with bar graph using transfer thresholds + warn_offset from alert config)
      - 3 cascading planes with color-coded health:
        - Utility: green (HE eligible), blue (online), orange (degraded), red (on battery)
        - UPS: green (normal/AVR states), yellow (commanded bypass), orange (fault-forced bypass), red (fault), gray (off). AVR boost/buck/passthrough detection for SMT.
        - Output: green (active), yellow (load elevated 60-80%), orange (load critical >80% or overload), red (off + fault), gray (off commanded)
- [x] Telemetry page (historical charts with uPlot, date range picker)
- [x] Events page (filterable by severity + category toggle chips, dynamic category list from data)
- [x] Commands page — grouped by operational use:
      - Power Control: bypass (state-aware toggle with voltage window + battery warning in modal), shutdown (dry-run + full with modal)
      - Diagnostics: battery test, runtime calibration (with charge time + bad battery warnings), beep test (short + continuous with stop)
      - Alarm & Fault Management: mute/unmute, clear faults
      - All commands use confirmation modals; results shown as toasts
- [x] UPS Config page (register table with edit-in-place, skeleton matches real data size)
- [x] App Config page (settings forms)
- [x] Shutdown Config page (target/group CRUD, drag-to-reorder, test button)
- [x] Weather Config page (location, zones, thresholds, enable/disable)
- [ ] Setup wizard (first-run flow):
      - Change `on_first_run` from `CFG_FIRST_RUN_EXIT` to `CFG_FIRST_RUN_CONTINUE`
      - Daemon starts with defaults on first run, no exit, no manual config editing
      - Web UI serves immediately — the setup wizard IS the web UI
      - Flow: set admin password → configure serial port → UPS auto-detects → optional Pushover/weather/shutdown
      - Need "reconnect UPS" API endpoint so user doesn't restart daemon after changing serial port
      - Gated behind `auth_is_setup()` check — if no password set, redirect to wizard
- [x] Build pipeline: Vite builds to static dir, Makefile copies to daemon asset path

### Phase 6: Polish + Hardening -- IN PROGRESS

- [x] Deploy script (`deploy.sh`) with modes: full, build, restart, frontend, install-service
- [x] Deployment documentation (`DEPLOY.md`)
- [x] Systemd service file (updated for new binary layout)
- [x] API restart endpoint (`POST /api/restart`) using c-utils `appguard_restart()`
- [x] UTC timestamps throughout (events, telemetry, retention, auth)
- [x] Granular event system — per-bit state transition events replacing generic "UPS Status Change"
- [x] Configurable Pushover severity threshold (`push.min_severity` app config)
- [ ] Cross-compilation toolchain (ARM target from x86 dev machine)
- [ ] Serial port forwarding for dev (socat TCP bridge)
- [ ] Telemetry retention policies and DB vacuuming
- [ ] Error recovery (serial port reconnection, DB corruption handling)
- [ ] API rate limiting
- [ ] Comprehensive testing

### Data Capture Gaps

Items not currently captured that should be addressed:

- [ ] UPS config register change history — `ups_config` table exists but isn't populated on reads; should snapshot on change
- [ ] Telemetry connectivity gaps — no record of when UPS was unreachable or how long
- [ ] Outlet group states over time — MOG/SOG0/SOG1 state changes not tracked in telemetry
- [ ] Cumulative energy consumption — `output_energy_wh` from dynamic block not recorded
- [ ] Weather forecast data — severe/clear transitions go to events but raw NWS forecast data isn't stored
- [ ] Auth/session audit log — login attempts, token creation not logged

---

## Upcoming Work

### SMT Driver Verification

SMT750RM2U hardware arriving soon for testing:
- Verify all register reads/commands against actual hardware
- Populate config register descriptors (currently empty TODO)
- Confirm HE mode support (bit 13 marked as supported for SMX/SMT in register map)
- Verify AVR voltage differential thresholds for boost/buck detection (currently 2V)
- Test SOG availability per model via `sog_config` register

### Weather Subsystem — Generic Parameter Control

The weather subsystem currently hardcodes frequency tolerance as the parameter it controls for HE inhibit. This should be generalized:

- **Any writable UPS register** should be selectable as the weather-controlled parameter, not just frequency tolerance. Configure register name + severe value + normal value in weather config.
- **Read/save/restore pattern**: before writing the severe value, read and save the current register value. On restore, write back the saved value (what the register was before weather touched it), OR fall back to a configured default. This prevents the weather system from overwriting a manually-set custom value with a generic "normal" setting.
- Makes the weather system work for any UPS and any parameter, not just freq tolerance on the SRT.

### Bypass Voltage Thresholds

Bypass voltage window (e.g. 90V–140V) is not exposed via Modbus — it's only configurable through the UPS LCD. Register probing confirmed the values don't appear in any documented or undocumented register range that changes when LCD settings are modified.

Current approach: stored as app config keys (`bypass.voltage_high`, `bypass.voltage_low`) that the user sets to match their LCD. Surfaced in the bypass enable modal as context for the operator.

### Dashboard Power Flow Visualization

Future enhancement: connected graphic/lines between the 3 cascading planes showing energy flow from utility through UPS to output. Lines change color/thickness based on plane health states. Animated flow direction. Deferred until both topologies are running and validated.

---

## Dependencies

### c-utils (static library)
- `sqlite3` — embedded database
- `libcurl` — Pushover notifications (extends to NWS API in airies-ups)
- `cJSON` — lightweight JSON parser (vendored, single .c/.h, MIT)

### airies-ups (daemon + CLI)
- `libc-utils.a` — application framework (DB, logging, config, push, lifecycle)
- `libmodbus` — Modbus RTU communication
- `cJSON` — JSON API request/response (vendored via c-utils or separately)
- `libmicrohttpd` — embedded HTTP server
- `libcrypto` (OpenSSL) — auth password hashing
- Frontend: React 19, Tailwind 4, Vite 8, uPlot 1.6 (dev-machine only, output is static assets)

### Retired
- `log.h` — replaced by c-utils logging subsystem
- `pushover.h` — replaced by c-utils push subsystem

---

## Resolved Questions

1. **HTTP server library** — `libmicrohttpd`. Battle-tested, handles threading, dual transport (TCP + unix socket).
2. **Charting library (frontend)** — uPlot. Fast, small, lightweight.
3. **TLS** — LAN-only, no HTTPS needed. Reverse proxy if required later.
