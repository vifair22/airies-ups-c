# airies-ups-c Implementation Plan

## Overview

Full C rewrite of airies-ups-rs. Direct Modbus RTU communication with APC SRT1000XLA over serial (`/dev/ttyUSB0`), replacing NUT entirely. Single interface for both data and control.

## Hardware

- **UPS:** APC SRT1000XLA (Smart-UPS SRT 1000VA / 900W)
- **Controller:** Raspberry Pi (upspi.internal.airies.net)
- **Interface:** USB-to-serial, `/dev/ttyUSB0`, 9600 8N1, Modbus slave ID 1
- **Outlet groups:** MOG + SOG0 + SOG1 (confirmed via reg 590 = 0x0006)

## Phase 0: Control Plane Verification — COMPLETE

All verified. See `APC_SRT_MODBUS_REFERENCE.md` for full results.

- Reg 1538 (OutletCommand) does NOT work over serial RTU
- Reg 1540 (SimpleSignalingCommand) works — `0x0001` = shutdown, ~60s fixed countdown
- Outlets stay off without AC, come back when mains returns
- Same behavior in both HE and Online modes
- Battery test, beeper test, clear faults, bypass toggle all verified
- Operating mode (reg 593) is a transient override, LCD config is authoritative
- Write timing: minimum 100ms between consecutive register writes

## Phase 1: MVP Application

### 1.1 — Project structure
```
airies-ups-c/
├── src/
│   ├── main.c          — entry point, arg parsing, main loop
│   ├── ups.c / ups.h   — Modbus connection, register reads, command writes
│   ├── shutdown.c / .h — SSH shutdown logic (UnRaid only), shutdown workflow
│   └── config.c / .h   — config file parsing
├── Makefile
├── config.ini          — runtime config (hosts, credentials, Pushover keys)
└── airies-ups.service  — systemd unit
```

### 1.2 — UPS communication layer (ups.c)
- `ups_connect()` — open Modbus RTU, set slave, configure timeouts
- `ups_read_status()` — bulk read status block (reg 0-26)
- `ups_read_dynamic()` — bulk read dynamic block (reg 128-171)
- `ups_read_inventory()` — bulk read inventory block (reg 516-595), called once at startup
- `ups_cmd_shutdown()` — write `0x0001` to reg 1540
- `ups_cmd_clear_faults()` — write `0x0200` to reg 1536
- `ups_cmd_battery_test()` — write `0x0001` to reg 1541
- `ups_cmd_mute_alarm()` — write `0x0004` to reg 1543
- `ups_close()` — cleanup

### 1.3 — Data we extract
- Battery charge % (reg 130, /512)
- Battery runtime seconds (reg 128-129, uint32)
- Battery voltage (reg 131, /32)
- Internal temperature (reg 135, /128) — inverter heatsink sensor
- UPS status bitfield (reg 0-1, uint32)
- Input voltage (reg 151, /64)
- Output load % (reg 136, /256)
- Output voltage (reg 142, /64)
- Output frequency (reg 144, /128)
- Output current (reg 140, /32)
- Efficiency (reg 154, /128, with special negative codes)
- Transfer reason (reg 2, enum)
- Low battery / shutdown imminent (reg 18, bit 1)
- Replace battery (reg 19, bit 1)
- Outlet group states (reg 3-14)
- Battery test status (reg 23)

### 1.4 — Monitor loop
- Poll status + dynamic blocks every 2s
- Compare current vs previous state for change detection
- On status change: log + Pushover notification
- On shutdown condition (OB + Sig reg 18 bit 1): trigger shutdown workflow
- Shutdown trigger also catches: UPS-initiated FSD, battery critical, etc.

### 1.5 — Shutdown workflow (2-phase)

**Phase 1 — Remote hosts (parallel via fork):**
- SSH shutdown UnRaid hosts: `sshpass -p <pass> ssh <user>@<host> powerdown`
- Ping-wait for each to go down (180s timeout, 15s grace)

**Phase 2 — UPS + local:**
- Write `0x0001` to reg 1540 (shutdown command, ~60s fixed countdown)
- Wait 5s
- `shutdown -h now` on Pi

### 1.6 — CLI
- No args: normal monitoring mode
- `--reboot-now`: interactive confirm, then run shutdown workflow
- `--status`: one-shot status dump and exit
- `--test-battery`: trigger battery self-test and exit

### 1.7 — Notifications (via c-pushover)
- Program started (with initial UPS status)
- UPS status change (OL→OB, OB→OL, alarm, fault, etc.)
- Battery depleted / shutdown triggered
- Battery test results

### 1.8 — Config file (INI format)
```ini
[ups]
device = /dev/ttyUSB0
baud = 9600
slave_id = 1

[pushover]
token = <app_token>
user = <user_key>

[shutdown]
unraid_hosts = server1,server2
unraid_user = root
unraid_pass = <password>
shutdown_timeout = 180
```

## Dependencies

- `libmodbus` (already installed on Pi)
- `libcurl` (required by c-pushover, need to verify installed on Pi)
- `gcc` (already installed on Pi)
- `sshpass` (for SSH shutdown, already installed)
- `c-log` — header-only logging (`/home/vifair/git/home/c-log/log.h`)
- `c-pushover` — Pushover notifications (`/home/vifair/git/home/c-pushover/pushover.h`)

## Open Questions

- [x] Exact Modbus bit pattern for shutdown trigger — Sig reg 18 bit 1 (ShutdownImminent)
- [x] Whether shutdown command works same as NUT `shutdown.reboot` — Yes, via reg 1540
- [x] Outlet command (reg 1538) — does NOT work over serial, use reg 1540 instead
- [x] Shutdown delay configurable? — No, reg 1540 uses fixed ~60s countdown
- [ ] Config format — going with INI
- [ ] Cross-compile on dev machine vs compile on Pi?
- [ ] Does Pi have libcurl-dev installed?
