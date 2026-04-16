# Deployment

airies-ups deploys to a Raspberry Pi via source sync + remote build. There is no cross-compilation — builds happen natively on the Pi.

## Target

| | |
|---|---|
| Host | `sysadmin@upspi.internal.airies.net` |
| Source dir | `/home/sysadmin/airies-ups` |
| c-utils dir | `/home/sysadmin/c-utils` |
| Service | `airies-ups.service` (systemd) |
| Web UI | `http://upspi.internal.airies.net:8080` |

## Prerequisites (Pi)

Build tools and libraries must be installed on the Pi:

```
gcc make libmodbus-dev libsqlite3-dev libcurl4-openssl-dev
libmicrohttpd-dev libssl-dev
```

The dev machine needs `bun` for frontend builds (not installed on the Pi).

## Quick deploy

```bash
./deploy.sh          # full: build frontend, sync, build on Pi, restart service
```

## Deploy script modes

| Command | What it does |
|---------|-------------|
| `./deploy.sh` or `./deploy.sh full` | Build frontend locally, rsync both repos, build c-utils + airies-ups on Pi, restart service |
| `./deploy.sh build` | Build frontend + sync + build, but don't restart the service |
| `./deploy.sh restart` | Restart the service only (no build) |
| `./deploy.sh frontend` | Rebuild frontend locally, sync, restart (skip C rebuild) |
| `./deploy.sh install-service` | Copy the service file to systemd and reload (one-time setup) |

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
ssh sysadmin@upspi.internal.airies.net "systemctl status airies-ups"

# Logs (follow)
ssh sysadmin@upspi.internal.airies.net "journalctl -u airies-ups -f"

# Restart
ssh sysadmin@upspi.internal.airies.net "sudo systemctl restart airies-ups"

# Install/update service file (after changing airies-ups.service)
./deploy.sh install-service
```

## First-time setup

1. Ensure Pi has build dependencies installed
2. Create the source directories on the Pi:
   ```bash
   ssh sysadmin@upspi.internal.airies.net "mkdir -p /home/sysadmin/airies-ups /home/sysadmin/c-utils"
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
