# airies-ups

UPS management platform — replaces APC PowerChute with a self-contained daemon plus web UI that talks to the hardware directly. One Raspberry Pi per UPS; one daemon per Pi; no NUT, no PowerChute, no cloud dependency.

## What it does

- **Direct hardware control.** Modbus RTU for APC Smart-UPS SMT and SRT lines; USB HID for APC Back-UPS and CyberPower PowerPanel families (built on a shared HID PDC core). No `upsd` / NUT shim in between.
- **Single-binary deployment.** The React frontend bundle and the SQLite migrations are embedded into the daemon binary at build time. The Pi only needs runtime libraries, a `config.yaml`, and the binary itself.
- **Web UI.** Dashboard, command panel, event log, full configuration surface, first-run setup wizard. Served from the daemon binary — no separate static-file path, no nginx.
- **Weather-aware High Efficiency mode.** Optionally toggles the UPS HE mode out of the way of forecast storms.
- **Orchestrated shutdown.** Drives a coordinated shutdown of dependent hosts when battery state warrants.
- **Pushover alerts.** Per-event severity routing.

## Repo layout

```
src/
  alerts/      alert engine and threshold dispatch
  api/         libmicrohttpd HTTP server, REST routes, embedded frontend
  cli/         airies-ups CLI (status, config, send-cmd)
  config/      bootstrap config (config.yaml) and DB-backed runtime config
  daemon/      airies-upsd entry point and lifecycle
  monitor/     poll loop, telemetry snapshots, retention sweeper
  shutdown/    dependent-host shutdown orchestrator
  ups/         driver registry, SMT/SRT Modbus drivers, APC + CyberPower HID adapters on shared hid_pdc_core
  weather/     weather lookup and HE mode policy
frontend/      React / TypeScript web UI (Vite + Tailwind, embedded into daemon)
migrations/    SQLite migration files (embedded at build time)
tests/         cmocka unit tests
docs/
  reference/   our notes derived from external material
  vendor/      raw external material (datasheets, USB-IF specs)
scripts/       build helpers
```

## Building

Native build for local development:

```bash
make             # release: frontend + tests + embed + binary
make debug       # debug build; skips frontend embed (Vite serves from disk)
make test        # C cmocka suite only
make coverage    # gcovr line-coverage report
make analyze     # cppcheck + stack-usage + gcc -fanalyzer
make lint        # clang-tidy
```

All build artifacts land under `build/` (per-variant subfolders: `build/release/`, `build/debug/`, `build/coverage/`). Binaries are at `build/airies-upsd` and `build/airies-ups`.

Cross-compile for aarch64 / Raspberry Pi:

```bash
make cross
```

See [DEPLOY.md](DEPLOY.md) for the sysroot rsync, toolchain install, and the multi-arch library list.

## Running

Local dev (no Pi needed):

```bash
./deploy.sh local                       # native debug build into ~/.local/share/airies-ups
cd ~/.local/share/airies-ups && ./airies-upsd
cd frontend && bun run dev              # Vite dev server on :5173, proxies /api -> :8080
```

Open `http://localhost:5173`. On first run the setup wizard walks through admin password, UPS detection, and optional Pushover.

Production deploy:

```bash
./deploy.sh full upspi                  # cross-compile + rsync to one host
./deploy.sh                             # all hosts in parallel
```

Master commits auto-deploy via GitLab CI. See [DEPLOY.md](DEPLOY.md) for the deploy script modes, host inventory, and the CI pipeline diagram.

### Docker

A multi-arch (`amd64` / `arm64`) image is built and pushed on every master commit:

```
registry.git.airies.net/vifair22/airies-ups-c:latest
```

```bash
docker run -d \
  --name airies-ups \
  --restart unless-stopped \
  -p 8080:8080 \
  -v airies-ups-state:/var/lib/airies-ups \
  --device=/dev/bus/usb \
  registry.git.airies.net/vifair22/airies-ups-c:latest
```

Replace `--device=/dev/bus/usb` with `--device=/dev/ttyUSB0` (or similar) for serial Modbus UPSes.

State (`config.yaml`, `app.db`) lives in the named volume — first run drops you into the setup wizard at `http://<host>:8080`.

The udev rules for FTDI Modbus adapters are baked into the image at `/usr/share/airies-ups/udev/99-airies-ups-ftdi.rules`. They have to be installed on the **host**, not the container — copy with `docker cp airies-ups:/usr/share/airies-ups/udev/99-airies-ups-ftdi.rules /etc/udev/rules.d/` and run `sudo udevadm control --reload && sudo udevadm trigger`.

## Documentation

| File | Purpose |
|------|---------|
| [README.md](README.md) | This file — what it is, build, run. |
| [Manual.md](Manual.md) | Engineering-grade operations manual. Hardware, daily ops, troubleshooting, every page of the UI. |
| [DEPLOY.md](DEPLOY.md) | Build pipeline, deploy script, CI, runtime layout. |
| [driver-api.md](driver-api.md) | UPS driver contract — read first if writing or maintaining a driver. |
| [docs/reference/apc-smt-modbus.md](docs/reference/apc-smt-modbus.md) | Our notes on APC SMT Modbus registers and firmware quirks. |
| [docs/reference/apc-srt-modbus.md](docs/reference/apc-srt-modbus.md) | Same for the APC SRT line. |
| [docs/vendor/](docs/vendor/) | Raw external material — APC datasheets, USB-IF HID Power Device specs. |

## License

GPL-3.0 — see [LICENSE](LICENSE).
