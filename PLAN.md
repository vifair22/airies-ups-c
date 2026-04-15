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
- Feeds state changes to event journal
- Triggers alert engine on state transitions
- Triggers shutdown orchestrator on shutdown condition

### Alert Engine

Current hysteresis-based system, extended:
- Alert definitions stored in DB (threshold, deadband, severity)
- Default alert set created on first run
- Configurable via web UI
- Fires events to journal + Pushover

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
| Dashboard | Live UPS status, key metrics (voltage, load, charge, runtime, frequency, temp), outlet states, weather status, HE mode state |
| Telemetry | Historical charts (selectable time range, selectable metrics). Voltage, load, charge, runtime, temperature over time. |
| Events | Filterable event journal. Severity, category, timestamp, title, message. |
| Commands | Command panel: shutdown workflow (with dry-run), battery test, runtime cal, bypass toggle, freq tolerance, mute/beep, clear faults. All gated by driver capabilities. |
| Config: UPS | Read/write UPS config registers. Grouped by category (transfer voltages, outlet delays, load shed, battery test interval, sensitivity). Shows current vs last-known values. |
| Config: App | App settings: serial port, poll intervals, Pushover config, admin password change. |
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

### Phase 0: c-utils Library

Build the shared C application framework (`~/git/home/c-utils`). Static library (`libc-utils.a`) with headers. This is a standalone project used by airies-ups and future C apps.

**Subsystems:**
- [ ] **DB** — SQLite wrapper, thread-safe access, WAL mode
- [ ] **Migrations** — numbered SQL files, SHA256 checksum tracking, savepoint-per-migration rollback, two-tier (lib + app)
- [ ] **Config** — two-tier: core bootstrap (DB path, HTTP port, log level) via CLI flags/defaults + app-level config in DB with registration API (name, type, default, description), mutable at runtime
- [ ] **Logging** — colored console + async SQLite persistence, log levels (debug/info/warn/error), auto-cleanup (configurable retention)
- [ ] **Pushover** — DB-persisted notification queue, retry with exponential backoff, message splitting
- [ ] **AppGuard** — lifecycle manager: ordered init (config → DB → migrations → logging → push), signal handlers, graceful shutdown in reverse order
- [ ] **Error loop detector** — consecutive error tracking with normalization, threshold + cooldown + callback
- [ ] Makefile (builds `libc-utils.a`), tests (cmocka), static analysis

### Phase 1: Foundation

Restructure airies-ups for the new architecture. No new features — everything that works today keeps working.

- [ ] Integrate c-utils (link `libc-utils.a`, replace `log.h` and `pushover.h` with c-utils subsystems)
- [ ] Split into daemon (`airies-upsd`) + CLI (`airies-ups`) binaries
- [ ] Migrate app config from INI file to c-utils DB config (register keys at init, import INI on first run)
- [ ] Implement HTTP server in daemon (embedded, minimal — just enough to serve API + static files)
- [ ] Implement REST API layer (JSON request/response via cJSON)
- [ ] Serve API over both TCP (web UI) and unix socket (CLI)
- [ ] Route CLI through API (CLI is pure API client, no direct Modbus)
- [ ] Auth system (admin user/password, hashed in DB, token-based sessions)
- [ ] Makefile restructure for two binaries + c-utils linkage + frontend build integration

### Phase 2: Data Layer

Build the storage and retrieval infrastructure.

- [ ] Telemetry recording (monitor loop writes to SQLite, configurable sample interval)
- [ ] Telemetry downsampling/retention (keep full resolution for 24h, downsample older data, configurable retention)
- [ ] Event journal (replace ad-hoc log_msg + Pushover with structured events)
- [ ] Pushover becomes a subscriber to the event journal (fires on configurable severity levels)
- [ ] Config register descriptors in SRT + SMT drivers
- [ ] UPS config snapshot read/store
- [ ] Config register read/write API endpoints

### Phase 3: Shutdown Orchestration

Replace hardcoded shutdown logic with the configurable system.

- [ ] DB schema for targets and groups
- [ ] Target execution engine (SSH key, SSH password, custom command)
- [ ] Group execution engine (sequential groups, parallel within group)
- [ ] Final phase: UPS shutdown command + self-shutdown
- [ ] Dry-run mode
- [ ] Per-target status reporting via API
- [ ] API endpoints for CRUD on targets and groups
- [ ] Migration: import existing config.ini shutdown targets into DB on first run

### Phase 4: Weather Integration

Port weather monitor from Python to C, integrated into daemon.

- [ ] NWS API client (libcurl, JSON parsing — need a lightweight JSON parser, maybe cJSON)
- [ ] Alert zone polling
- [ ] Forecast/wind polling with gridpoint resolution
- [ ] HE inhibit/restore logic (port existing state machine)
- [ ] Weather config in DB
- [ ] Weather status + history in event journal
- [ ] API endpoints for weather status and config

### Phase 5: Web Frontend

React + Tailwind SPA, built on dev machine, static bundle served by daemon.

- [ ] Project setup (Vite + React + Tailwind)
- [ ] Auth flow (login page, token management)
- [ ] Dashboard page (live status, key metrics, outlet states, weather badge)
- [ ] Telemetry page (historical charts — lightweight charting lib)
- [ ] Events page (filterable table with severity icons)
- [ ] Commands page (action buttons, confirmation dialogs, status feedback)
- [ ] UPS Config page (register table with edit-in-place)
- [ ] App Config page (settings forms)
- [ ] Shutdown Config page (target/group CRUD, drag-to-reorder, test button)
- [ ] Weather Config page (location, zones, thresholds, enable/disable)
- [ ] Setup wizard (first-run flow)
- [ ] Build pipeline: Vite builds to static dir, Makefile copies to daemon asset path

### Phase 6: Polish + Hardening

- [ ] Cross-compilation toolchain (ARM target from x86 dev machine)
- [ ] Serial port forwarding for dev (socat TCP bridge)
- [ ] Systemd service files for daemon
- [ ] Telemetry retention policies and DB vacuuming
- [ ] Error recovery (serial port reconnection, DB corruption handling)
- [ ] API rate limiting
- [ ] Graceful shutdown (drain in-flight requests, close DB cleanly)
- [ ] Comprehensive testing

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
- HTTP server library — TBD (see open questions)
- Frontend: React, Tailwind, Vite (dev-machine only, output is static assets)

### Retired
- `log.h` — replaced by c-utils logging subsystem
- `pushover.h` — replaced by c-utils push subsystem

---

## Open Questions

1. **HTTP server library** — `libmicrohttpd` is battle-tested and handles TLS, threading, etc. But it's a real dependency. A hand-rolled HTTP/1.1 server is feasible for this scope (single-client web UI, low request rate) and keeps deps minimal. Preference?
2. **Charting library (frontend)** — lightweight options: Chart.js, uPlot (fast, small), Recharts (React-native). Preference?
3. **TLS** — does the web UI need HTTPS, or is it behind a reverse proxy / LAN-only?
4. **Telemetry retention** — how far back do you want to keep data? 30 days? 90? A year? This affects DB size and query performance.
5. **Serial port scanning** — for the setup wizard, do you want the daemon to enumerate `/dev/ttyUSB*` and probe each for a Modbus response, or just present a dropdown of available ports?
