# Deployment

airies-ups deploys to a Raspberry Pi via source sync + remote build. There is no cross-compilation — builds happen natively on the Pi.

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

## Prerequisites (Pi)

Build tools and libraries must be installed on the Pi:

```
gcc make libmodbus-dev libsqlite3-dev libcurl4-openssl-dev
libmicrohttpd-dev libssl-dev
```

The dev machine needs `bun` for frontend builds (not installed on the Pi).

## Quick deploy

```bash
./deploy.sh              # full deploy to all hosts
./deploy.sh full upspi   # deploy to upspi only
./deploy.sh full upspi2  # deploy to upspi2 only
```

## Deploy script modes

| Command | What it does |
|---------|-------------|
| `./deploy.sh [full] [target]` | Build frontend locally, rsync both repos, build on Pi(s), restart service |
| `./deploy.sh build [target]` | Build frontend + sync + build, but don't restart the service |
| `./deploy.sh restart [target]` | Restart the service only (no build) |
| `./deploy.sh frontend [target]` | Rebuild frontend locally, sync, restart (skip C rebuild) |
| `./deploy.sh install-service [target]` | Copy the service file to systemd and reload (one-time setup) |

Target is `all` (default), `upspi`, or `upspi2`.

## What gets synced

The script rsyncs the full source tree for both `c-utils` and `airies-ups`, excluding:
- `.git`, `build/`, `node_modules/`, `.venv/`
- `*.db`, `*.db-shm`, `*.db-wal` (database files are persistent on the Pi)
- `config.yaml` (never overwritten — Pi has its own config)
- `he_inhibit` (runtime state file)

The pre-built `frontend/dist/` IS synced since bun is not on the Pi.

## Runtime layout

The daemon runs from the source directory (`/home/sysadmin/airies-ups`) with all paths relative to the working directory:

```
/home/sysadmin/airies-ups/
  build/airies-upsd       # daemon binary (systemd ExecStart)
  build/airies-ups        # CLI binary
  config.yaml             # app config (serial port, HTTP port, Pushover creds)
  app.db                  # SQLite database (created on first run)
  migrations/             # SQL migration files (run on startup)
  frontend/dist/          # static frontend bundle (served by daemon)
```

## Configuration

`config.yaml` is the bootstrap config (needed before DB starts):

```yaml
db:
  path: app.db
ups:
  device: /dev/ttyUSB0
  baud: 9600
  slave_id: 1
http:
  port: 8080
  socket: /tmp/airies-ups.sock
pushover:
  token: <pushover app token>
  user: <pushover user key>
```

Runtime settings (poll intervals, alert thresholds, etc.) are stored in the database and configurable via the web UI.

## Service management

```bash
# Status
ssh sysadmin@upspi.internal.airies.net  # or upspi2 "systemctl status airies-ups"

# Logs (follow)
ssh sysadmin@upspi.internal.airies.net  # or upspi2 "journalctl -u airies-ups -f"

# Restart
ssh sysadmin@upspi.internal.airies.net  # or upspi2 "sudo systemctl restart airies-ups"

# Install/update service file (after changing airies-ups.service)
./deploy.sh install-service
```

## First-time setup

1. Ensure Pi has build dependencies installed
2. Create the source directories on the Pi:
   ```bash
   ssh sysadmin@upspi.internal.airies.net  # or upspi2 "mkdir -p /home/sysadmin/airies-ups /home/sysadmin/c-utils"
   ```
3. Deploy:
   ```bash
   ./deploy.sh build
   ```
4. Create `config.yaml` on the Pi with the correct serial device and credentials
5. Install the service file:
   ```bash
   ./deploy.sh install-service
   sudo systemctl enable airies-ups.service
   ```
6. Start:
   ```bash
   ./deploy.sh restart
   ```

## Troubleshooting

**Port 8080 already in use**: A stale process from a previous crash may hold the port. Check with `ss -tlnp | grep 8080` and kill it.

**UPS not connecting**: Verify `/dev/ttyUSB0` exists and the `sysadmin` user has permission (`dialout` group). Check `config.yaml` has the correct device path.

**Service crash-loops**: Check `journalctl -u airies-ups -n 50` for the error. Common causes: wrong serial device, port conflict, missing `config.yaml`.
