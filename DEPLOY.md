# Deployment

airies-ups deploys to a Raspberry Pi via source sync + remote build. There is no cross-compilation — builds happen natively on the Pi.

The frontend (React/Vite) and SQL migrations are embedded into the daemon binary at build time. A single binary serves the API, web UI, and manages its own schema with no external files needed beyond `config.yaml` and the SQLite database.

## Targets

| Name | Host | UPS | Web UI |
|------|------|-----|--------|
| upspi | `sysadmin@upspi.internal.airies.net` | APC SRT (Modbus RTU) | `http://upspi.internal.airies.net:8080` |
| upspi2 | `sysadmin@upspi2.internal.airies.net` | APC Back-UPS ES 600M1 (USB HID) | `http://upspi2.internal.airies.net:8080` |

Both use the same directory layout:

| | |
|---|---|
| Source dir | `/home/sysadmin/airies-ups` |
| c-utils dir | `/home/sysadmin/c-utils` |
| Service | `airies-ups.service` (systemd) |

## Prerequisites

**Pi** — build tools and libraries:

```
gcc make libmodbus-dev libsqlite3-dev libcurl4-openssl-dev
libmicrohttpd-dev libssl-dev
```

**Dev machine** — frontend build chain:

```
bun brotli gzip xxd
```

**USB HID (Back-UPS)**: The `sysadmin` user needs access to `/dev/hidrawN`. Add a udev rule:

```bash
echo 'SUBSYSTEM=="hidraw", ATTRS{idVendor}=="051d", MODE="0660", GROUP="plugdev"' \
  | sudo tee /etc/udev/rules.d/99-apc-ups.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

## Build pipeline

`make all` runs the full pipeline locally:

```
frontend          → bun install && bun run build (Vite bundle)
frontend-test     → vitest run (221 tests)
embed-frontend    → gzip + brotli compress, xxd into C byte arrays
embed-migrations  → embed_sql.sh converts migrations/*.sql into C arrays
test              → C unit tests (cmocka)
_build            → gcc release binary with embedded frontend + migrations
```

The frontend embed step generates `build/embedded_assets.c` containing raw, gzip, and brotli variants of each frontend file. The HTTP server does content negotiation at serve time — clients that accept brotli or gzip get pre-compressed responses with zero runtime compression cost.

The migration embed step generates `build/migrations_compiled.c` from the `migrations/` directory using c-utils' `embed_sql.sh`. The SQL files remain the source of truth — the generated C file is a build artifact.

`make debug` skips the frontend entirely and serves files from `frontend/dist/` on disk (for Vite dev server hot-reload).

## Quick deploy

```bash
./deploy.sh              # full deploy to all hosts
./deploy.sh full upspi   # deploy to upspi only
./deploy.sh full upspi2  # deploy to upspi2 only
```

## Deploy script modes

| Command | What it does |
|---------|-------------|
| `./deploy.sh [full] [target]` | Build + test locally, rsync source + embedded assets, build on Pi, restart |
| `./deploy.sh build [target]` | Build + test + sync + remote build, but don't restart the service |
| `./deploy.sh restart [target]` | Restart the service only (no build) |
| `./deploy.sh install-service [target]` | Copy the service file to systemd and reload (one-time setup) |

Target is `all` (default), `upspi`, or `upspi2`.

## What gets synced

The script rsyncs the full source tree for both `c-utils` and `airies-ups`, excluding:
- `.git`, `build/`, `node_modules/`, `.venv/`, `frontend/dist/`, `migrations/`
- `*.db`, `*.db-shm`, `*.db-wal` (database files are persistent on the Pi)
- `config.yaml` (never overwritten — Pi has its own config)
- `he_inhibit` (runtime state file)

After syncing source, the pre-generated `build/embedded_assets.c` and `build/migrations_compiled.c` are synced separately — the Pi doesn't need bun, brotli, or the frontend toolchain.

## Pre-deploy verification

Run the full analysis and lint suite before deploying:

```bash
# Full build pipeline (frontend + tests + embed + C tests + release binary)
make all

# C: compile check, stack usage, gcc-fanalyzer, cppcheck
make analyze

# C: clang-tidy
make lint
```

## Runtime layout

The daemon runs from the source directory (`/home/sysadmin/airies-ups`) with all paths relative to the working directory:

```
/home/sysadmin/airies-ups/
  build/airies-upsd       # daemon binary (frontend + migrations embedded)
  build/airies-ups        # CLI binary
  config.yaml             # bootstrap config
  app.db                  # SQLite database (created on first run)
```

## Configuration

`config.yaml` is the bootstrap config (needed before DB starts). UPS connection details are configured through the web UI setup wizard on first run.

Serial (Modbus RTU):
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

USB (HID):
```yaml
db:
  path: app.db
ups:
  conn_type: usb
  usb_vid: 051d
  usb_pid: 0002
http:
  port: 8080
  socket: /tmp/airies-ups.sock
pushover:
  token:
  user:
```

Runtime settings (poll intervals, alert thresholds, shutdown config, weather, etc.) are stored in the database and configurable via the web UI. Changes auto-restart the daemon.

## Service management

```bash
# Status
ssh sysadmin@upspi.internal.airies.net "systemctl status airies-ups"

# Logs (follow)
ssh sysadmin@upspi.internal.airies.net "journalctl -u airies-ups -f"

# Restart
ssh sysadmin@upspi.internal.airies.net "sudo systemctl restart airies-ups"
```

Replace `upspi` with `upspi2` for the second target.

## First-time setup

1. Ensure Pi has build dependencies installed (and udev rule for USB HID)
2. Create the source directories on the Pi:
   ```bash
   ssh sysadmin@<host> "mkdir -p /home/sysadmin/airies-ups /home/sysadmin/c-utils"
   ```
3. Deploy:
   ```bash
   ./deploy.sh build <target>
   ```
4. Create a minimal `config.yaml` on the Pi (just db + http, no UPS config needed):
   ```bash
   ssh sysadmin@<host> "cat > /home/sysadmin/airies-ups/config.yaml << 'EOF'
   db:
     path: app.db
   http:
     port: 8080
     socket: /tmp/airies-ups.sock
   EOF"
   ```
5. Install and enable the service:
   ```bash
   ./deploy.sh install-service <target>
   ```
6. Start:
   ```bash
   ./deploy.sh restart <target>
   ```
7. Open the web UI — the setup wizard will guide you through setting the admin password, detecting and configuring the UPS connection, and optional Pushover setup.

## Troubleshooting

**Port 8080 already in use**: A stale process from a previous crash may hold the port. Check with `ss -tlnp | grep 8080` and kill it.

**UPS not connecting (serial)**: Verify `/dev/ttyUSB0` exists and the `sysadmin` user has permission (`dialout` group).

**UPS not connecting (USB HID)**: Verify the device is visible with `lsusb | grep 051d` and that `/dev/hidraw0` is accessible by `sysadmin` (check the udev rule above).

**Service crash-loops**: Check `journalctl -u airies-ups -n 50` for the error. Common causes: wrong device path, port conflict, missing `config.yaml`.
