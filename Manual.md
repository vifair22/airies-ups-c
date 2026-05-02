# airies-ups Manual

Operations manual for `airies-ups`. Covers the system from sysadmin and operator perspectives — how it's structured, what it does at runtime, how to configure it, what every page of the web UI does, and where to look when something is wrong.

For driver internals, see [driver-api.md](driver-api.md). For build instructions, see the [README](README.md).

---

## 1. System overview

```
+--------------------------+         +----------------------------+
|  Raspberry Pi (one per)  |         |   Operator workstation     |
|                          |         |                            |
|  +-------------------+   |  HTTP   |   browser  ->  Web UI      |
|  | airies-upsd       |<--+---------+   ssh      ->  CLI / logs  |
|  |  - HTTP/JSON API  |   |         |                            |
|  |  - SQLite DB      |   |         +----------------------------+
|  |  - Monitor thread |   |
|  |  - Alert engine   |   |
|  |  - Shutdown orch. |   |
|  |  - Weather poller |   |
|  +---------+---------+   |
|            |             |
|     Modbus RTU / USB HID |
|            |             |
|         +--+--+          |
|         | UPS |          |
|         +-----+          |
+--------------------------+
```

One daemon per UPS. Each Pi owns its UPS exclusively over a serial USB cable (Modbus) or USB HID interface. The daemon binds the API on TCP port 8080 and a unix socket for the local CLI.

### Components inside the daemon

| Component | Source | Responsibility |
|-----------|--------|----------------|
| HTTP API | `src/api/` | libmicrohttpd; serves the embedded React bundle and the JSON API on TCP + unix socket |
| Monitor thread | `src/monitor/` | Two loops: slow loop polls the full UPS state at the configured cadence (default 5 s) for everything except power-state events; fast loop polls status + transfer reason at 200 ms and owns sub-second power events (on-battery, output-off, bypass, fault, overload). HID drivers run slow-only. Snapshot/config/retention work runs on the slow loop. |
| Alert engine | `src/alerts/` | Hysteresis-based threshold checks on each monitor poll; fires events only on state transitions |
| Shutdown orchestrator | `src/shutdown/` | Coordinates shutdown of dependent hosts when battery state warrants |
| Weather + HE mode | `src/weather/` | Optional storm-aware toggling of UPS High Efficiency mode |
| Driver registry | `src/ups/` | Vtable-based driver dispatch — see [driver-api.md](driver-api.md) |
| Config | `src/config/` | Bootstrap config from `config.yaml`; everything else lives in SQLite and is editable from the UI |

### Two binaries

| Binary | Role |
|--------|------|
| `airies-upsd` | Daemon. Long-running, owns the UPS, serves the API, manages SQLite. |
| `airies-ups`  | CLI. Talks to the daemon over its unix socket. Subcommands: `status`, `events`, `telemetry`, `cmd`. |

---

## 2. Supported hardware

| Family | Transport | Driver | Verified models | Notes |
|--------|-----------|--------|------------------|-------|
| APC Smart-UPS SRT (online double-conversion) | Modbus RTU over USB-serial | `src/ups/ups_srt.c` | SRT1000XLA (FW 16.5), SRT 2200 | Full feature set: bypass, deep test, frequency tolerance, outlet groups |
| APC Smart-UPS SMT (line-interactive) | Modbus RTU over USB-serial | `src/ups/ups_smt.c` | SMT1500RM2UC (FW 04.1, ModbusMapID 00.5) | No bypass; no Green Mode write over Modbus (empirically — see project notes) |
| APC Back-UPS / Smart-UPS HID | USB HID | `src/ups/ups_apc_hid.c` (+ `hid_pdc_core.c`) | Back-UPS ES 600M1 | APC vendor page (sensitivity, lights/beeper test, batt repl date); deep runtime calibration intentionally omitted (firmware rejects it) |
| CyberPower PowerPanel HID | USB HID | `src/ups/ups_cyberpower_hid.c` (+ `hid_pdc_core.c`) | CP1500PFCLCD | Standard HID PDC only; no vendor extensions in this driver yet |

Hardware reference material lives under `docs/`:

- `docs/reference/apc-smt-modbus.md` — SMT register map, bit definitions, firmware quirks
- `docs/reference/apc-srt-modbus.md` — SRT register map and quirks
- `docs/vendor/apc/` — original APC datasheets and Application Note 176
- `docs/vendor/usb-if/` — USB-IF HID Power Device class specification

---

## 3. Deployment topology

One daemon per UPS, one host (typically a Raspberry Pi) per daemon. Each host owns its UPS exclusively over USB-serial (Modbus RTU) or USB HID.

Daemons are independent. There is no inter-daemon coordination — orchestrated shutdown is between an `airies-upsd` and the hosts it shuts down (its dependents), not between daemons. Multiple UPSes mean multiple daemons, each with its own dependent inventory.

Web UI per-host: `http://<hostname>:8080`.

---

## 4. Configuration

There are two layers.

### 4.1 Bootstrap — `config.yaml`

Read once at daemon startup, before SQLite is opened. Just enough to find the database, bind the HTTP server, and (optionally) describe the UPS connection. Everything else is in the DB.

Path on a `.deb`-installed host: `/var/lib/airies-ups/config.yaml`. Docker installs see the same path inside the container; map it via the `airies-ups-state` volume.

Modbus RTU example:

```yaml
db:
  path: app.db
ups:
  conn_type: serial
  device: /dev/ttyUSB0
  baud: 9600
  slave_id: 1
http:
  port: 8080
  socket: /tmp/airies-ups.sock
pushover:
  token:
  user:
```

USB HID example:

```yaml
db:
  path: app.db
ups:
  conn_type: usb
  usb_vid: "051d"
  usb_pid: "0002"
http:
  port: 8080
  socket: /tmp/airies-ups.sock
```

The `ups:` block can be omitted on first install — the setup wizard fills it in from the web UI.

### 4.2 Runtime — SQLite

`app.db` lives next to `config.yaml` (`/var/lib/airies-ups/app.db` on `.deb` installs). All runtime configuration is here, editable from the web UI:

| Concern | Page | What you set |
|---------|------|--------------|
| App-wide | `/config/app` | Poll interval, telemetry interval, retention windows, alert thresholds, log level |
| UPS registers | `/config/ups` | Driver-exposed config registers (output voltage, transfer points, sensitivity, outlet groups, etc.) |
| Shutdown | `/config/shutdown` | Dependent host inventory, shutdown groups, trigger thresholds |
| Weather | `/config/weather` | Location, provider credentials, HE mode policy |

Schema lives in `migrations/*.sql`. Migrations are embedded into the binary at build time and run automatically at startup.

---

## 5. First-run setup wizard

On a fresh install (`setup.first_run_continue` not yet set) the web UI redirects every request to the wizard until it completes:

1. **Admin password** — sets the credential checked by `/api/auth/login`.
2. **UPS connection** — auto-detects serial ports / USB devices via `/api/setup/ports`, lets you test (`/api/setup/test`) before committing.
3. **Pushover (optional)** — token + user key for alert routing.
4. **Done** — flips the first-run flag, redirects to the dashboard.

After the wizard, the same connection settings persist in `config.yaml` (so they survive a full DB wipe) and runtime settings live in the DB.

---

## 6. Web UI tour

| Page | Route | What it shows |
|------|-------|---------------|
| Dashboard | `/` | Live status: input/output volts, load %, battery %, runtime, mode (online/battery/bypass), alerts. Power-flow diagram showing where energy is moving. |
| Events | `/events` | Time-ordered event log: alerts, mode changes, command executions, errors. Filterable by severity. |
| Commands | `/commands` | Operator command panel: self-test, deep test, outlet group control, beeper, reboot. Driver-declared commands only — the page hides commands the driver doesn't expose. |
| About | `/about` | Identity (model, serial, firmware), driver name, capability set, raw register dump for debugging. |
| UPS Registers | `/config/ups` | Driver-exposed config registers. Spec-driven; the UI builds itself from the driver's `config_regs[]` list. |
| App Settings | `/config/app` | Poll cadence, telemetry retention, alert thresholds, log level. |
| Shutdown | `/config/shutdown` | Dependent host inventory and shutdown groups. |
| Weather | `/config/weather` | Provider config, HE-mode policy, current forecast. |
| Setup | `/setup` | First-run wizard (auto-redirected to). |
| Login | `/login` | Auth screen. Token persisted in `localStorage`. |

Page sources are at `frontend/src/pages/<Name>.tsx` with co-located `.test.tsx` Vitest specs.

---

## 7. CLI

```bash
airies-ups status              # one-shot status: model, mode, load, battery, runtime
airies-ups events [-n N]       # tail the event log
airies-ups telemetry [-n N]    # tail telemetry snapshots
airies-ups cmd <name> [args]   # send a driver command (same as the Commands page)
```

The CLI talks to the daemon over `/tmp/airies-ups.sock`. Run it on the Pi (or via SSH).

---

## 8. Alert engine

State-transition alerts only — the engine fires once per state edge, never spams on every poll. Tracked conditions (`src/alerts/alerts.h`):

| Condition | Threshold source | Severity |
|-----------|------------------|----------|
| Overload | UPS status flag | error |
| Fault | UPS status flag | critical |
| Battery replace required | UPS status flag | warning |
| Input voltage high / low | `transfer_high/low` from UPS registers, with `voltage_warn_offset` deadband | warning |
| Load high | `load_high_pct` from app config | warning |
| Battery low | `battery_low_pct` from app config | critical |
| Error register transitions | UPS general / power / battery error registers | warning |

Notifications route through the configured channel (currently Pushover; in-app event log always populated). On daemon startup the alert state is seeded from the most recent persisted snapshot so previously-active conditions don't re-fire.

---

## 9. Monitor and telemetry

The monitor runs two loops with different responsibilities:

**Slow loop** (default 5 s, configurable via `monitor.poll_interval`):

1. `read_status` + `read_dynamic` from the driver
2. Snapshot evaluated by the alert engine (voltage thresholds, load, battery low, error registers)
3. Snapshot evaluated by the shutdown orchestrator
4. Detects HE-mode and self-test transitions
5. Persists `ups_status_snapshot` on change; runs daily config-register snapshot and `xfer_history` retention sweep

**Fast loop** (200 ms, Modbus drivers only — SMT/SRT):

1. Reads the status register and transfer-reason register every tick
2. Detects sub-second transitions on `ON_BATTERY`, `OUTPUT_OFF`, `BYPASS`, `FAULT`, `FAULT_STATE`, `OVERLOAD` and journals them immediately
3. Records every register-2 transition to `xfer_history` (7-day retention) so brief mains glitches that resolve before the slow loop lands still leave a forensic trail

HID drivers don't expose a separate fast read path — they run slow-loop-only. The 50 ms Modbus pacing wrapper (`src/ups/ups_modbus.{h,c}`) enforces a minimum gap between consecutive bus operations so the SMT management plane doesn't crash under combined slow + fast traffic.

---

## 10. Shutdown orchestration

Configured via `/config/shutdown`. You define:

- **Dependent hosts** — IP / hostname, SSH user, shutdown command.
- **Groups** — collections of hosts that shut down together.
- **Triggers** — battery percentage and runtime thresholds at which each group fires.

When the monitor poll meets a trigger, the orchestrator SSHes to each host in the group and runs its shutdown command. Groups fire in dependency order. The daemon does not shut down its own Pi by default — that's a separate group you can configure if desired.

Schema versions: `004_shutdown.sql` (initial) → `010_shutdown_v2.sql` (current group / target model).

---

## 11. Weather and HE mode

Optional. With a configured location and provider:

- The weather poller fetches the forecast on a schedule.
- If a storm matching the configured criteria is in the window, HE mode is disabled (so the UPS isn't sitting in line-interactive efficiency mode when transfer odds are elevated).
- HE mode is restored once the window clears.

Status visible at `/config/weather` (current forecast, last poll, current decision). Override available via `/api/weather/simulate` for testing.

---

## 12. Logs and diagnostics

### Where the logs go

```bash
ssh <admin>@<host> "journalctl -u airies-ups -f"          # follow
ssh <admin>@<host> "journalctl -u airies-ups -n 100"      # last 100 lines
ssh <admin>@<host> "journalctl -u airies-ups --since '1 hour ago'"
```

Log level is set in app config (`/config/app`) and applies to both the daemon's stdout (which systemd captures) and `journalctl`.

### Service control

```bash
ssh <admin>@<host> "systemctl status airies-ups"
ssh <admin>@<host> "sudo systemctl restart airies-ups"
ssh <admin>@<host> "sudo systemctl stop airies-ups"
```

Unit definition: `airies-ups.service` at the repo root (`Restart=on-failure`, `RestartSec=5`).

### Database access

```bash
ssh <admin>@<host> "sudo -u airies-ups sqlite3 /var/lib/airies-ups/app.db"
```

Useful tables: `events`, `telemetry`, `app_config`, `ups_config`, `shutdown_groups`, `shutdown_targets`, `weather_state`.

---

## 13. Troubleshooting

### Service crash-loops on start

```
journalctl -u airies-ups -n 50
```

Common causes: wrong `device` path in `config.yaml`; port 8080 already bound by a stale process; missing `config.yaml`. The daemon binds the listen socket with `SO_REUSEADDR` so a `systemctl restart` does not race the kernel's TIME_WAIT cleanup.

### UPS not connecting (Modbus / serial)

- Confirm the device path: `ls -l /dev/ttyUSB*`.
- Confirm the `airies-ups` system user is in the `dialout` group: `id airies-ups`. (The `.deb`'s `postinst` joins it automatically; only re-check if you've installed by hand or hit a permission error.)
- Confirm slave ID and baud match the UPS-side configuration.

### UPS not connecting (USB HID)

- Confirm the device is on the bus: `lsusb` (APC VID `051d`, CyberPower VID `0764`).
- Confirm hidraw is accessible by `airies-ups`: `ls -l /dev/hidraw*`.
- The `.deb` ships `99-airies-ups-ftdi.rules` (also in the repo root) and joins `airies-ups` to `plugdev`. If you're running outside the `.deb` (Docker, hand-built), copy the rule to `/etc/udev/rules.d/` on the host, then `sudo udevadm control --reload-rules && sudo udevadm trigger`.

### Modbus reads erratic right after a config write

Expected — APC firmware needs a 200 ms quiet window after rapid-fire writes to the 1024–1073 block. The driver layer enforces this via `post_command_settle()` in `src/ups/ups.c`. If you see this from a custom write path, route it through the same helper. See `docs/reference/apc-smt-modbus.md` "Rapid-fire config register writes".

### Alerts re-firing every restart

Means the alert-state seed from the last persisted snapshot didn't load. Check that the most recent telemetry snapshot exists in `telemetry_*` tables; the seed reads from there.

### Frontend changes not visible after deploy

The frontend bundle is **embedded into the daemon binary at compile time**. A frontend-only edit will not show up after a `systemctl restart` — you have to rebuild and re-install the daemon (re-run the `.deb` install, or rebuild the Docker image).

### Port 8080 already in use locally

Stale `airies-upsd` from a previous dev session.

```bash
ss -tlnp | grep 8080
pkill -f airies-upsd
```

---

## 14. Where to find what

| Looking for | Path |
|-------------|------|
| API route handler | `src/api/routes/*.c` |
| Driver implementation | `src/ups/ups_<family>.c` |
| Driver contract | [driver-api.md](driver-api.md), `src/ups/ups_driver.h`, `src/ups/ups.h` |
| SQL schema | `migrations/NNN_*.sql` (embedded at build) |
| Web UI page | `frontend/src/pages/<Name>.tsx` |
| Web UI component | `frontend/src/components/<Name>.tsx` |
| Bootstrap config schema | `src/config/app_config.h` |
| Service unit | `airies-ups.service` |
| udev rule | `99-airies-ups-ftdi.rules` |
| CI pipeline | `.gitlab-ci.yml` |
| Local dev workflow | [README.md](README.md) §Development |
| Hardware reference | `docs/reference/apc-{smt,srt}-modbus.md`, `docs/vendor/` |
