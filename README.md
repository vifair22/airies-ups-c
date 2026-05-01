# airies-ups

[![pipeline](https://git.airies.net/vifair22/airies-ups-c/badges/master/pipeline.svg)](https://git.airies.net/vifair22/airies-ups-c/-/commits/master)
[![coverage](https://git.airies.net/vifair22/airies-ups-c/badges/master/coverage.svg)](https://git.airies.net/vifair22/airies-ups-c/-/commits/master)
[![release](https://git.airies.net/vifair22/airies-ups-c/-/badges/release.svg)](https://git.airies.net/vifair22/airies-ups-c/-/releases)
[![license](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)
[![C11](https://img.shields.io/badge/C-11-A8B9CC.svg?logo=c)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))
[![platforms](https://img.shields.io/badge/platform-linux--amd64%20%7C%20linux--arm64-lightgrey.svg)](#docker)

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

Multi-arch (`amd64` / `arm64`) images are published to `registry.git.airies.net/vifair22/airies-ups-c`. Tag scheme matches the Debian package release tracks:

| Tag | Refreshed | Use for |
|-----|-----------|---------|
| `:latest` | latest stable release | Production. Follows Docker community convention — *not* master. |
| `:v0.1.0` (etc.) | once, immutable | Pinning to a specific stable release |
| `:nightly` | every master push | Tracking the bleeding edge |
| `:<short_sha>` | once, immutable | Pinning to a specific commit |

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

### Debian package

For Debian trixie or derivatives (Ubuntu 24.04+, Raspberry Pi OS trixie), `.deb` packages for `amd64` and `arm64` are published to the project's [Releases page](https://git.airies.net/vifair22/airies-ups-c/-/releases). Two release tracks:

- **Nightly** — rolling snapshot of `master`, refreshed on every push. Stable URLs:
  - `https://git.airies.net/api/v4/projects/233/packages/generic/airies-ups/nightly/airies-ups_amd64.deb`
  - `https://git.airies.net/api/v4/projects/233/packages/generic/airies-ups/nightly/airies-ups_arm64.deb`
- **Stable** — tagged releases (`v0.1.0`, etc.). Each tag's `.deb` is immutable. Tags are created automatically by CI when the [`release_version`](release_version) file changes on `master` — bump the semver, push, the next pipeline tags `v<semver>` and publishes the stable release.

Install:

```bash
ARCH=$(dpkg --print-architecture)
curl -fLO "https://git.airies.net/api/v4/projects/233/packages/generic/airies-ups/nightly/airies-ups_${ARCH}.deb"
sudo apt install ./airies-ups_${ARCH}.deb
```

The package creates an `airies-ups` system user (joined to `dialout` and `plugdev` for UPS hardware access), drops the binaries in `/usr/bin/`, the systemd unit in `/usr/lib/systemd/system/`, the udev rule in `/usr/lib/udev/rules.d/`, and reserves `/var/lib/airies-ups/` for `config.yaml` + `app.db`. The setup wizard runs on first start at `http://<host>:8080`.

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
