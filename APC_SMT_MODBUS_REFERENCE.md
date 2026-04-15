# APC Smart-UPS SMT Modbus Reference

Baseline register map for **APC SMT750RM2U** (Smart-UPS SMT 750VA / 500W rack-mount) over **serial RTU**.

This document is derived from the APC/Schneider Electric register map document **990-9840** which covers SMT, SMX, SURTD, and SRT models under a single unified register table. Register addresses and scaling factors are identical to the SRT series. **All entries below require hardware verification.**

---

## Protocol

- **Transport:** Modbus RTU over serial (RJ50 port with 940-0625A cable to USB-to-serial adapter)
- **Baud:** 9600
- **Parity:** None
- **Data bits:** 8
- **Stop bits:** 1
- **Slave ID:** 1 (configurable via UPS LCD)
- **Function codes used:** FC03 (Read Holding Registers), FC06 (Write Single Register), FC16 (Write Multiple Registers)
- **Byte order:** Big-endian. 32-bit values are stored MSB-first across two consecutive 16-bit registers.
- **Modbus must be enabled** via UPS front panel LCD (Configuration menu)

---

## Key Differences from SRT

The SMT750RM2U is a **line-interactive** UPS, unlike the SRT which is **online double-conversion**. Expected differences:

| Feature | SRT1000XLA | SMT750RM2U (expected) |
|---------|------------|----------------------|
| Topology | Online double-conversion | Line-interactive |
| HE/ECO mode | Yes (status bit 13) | **No** — not applicable to line-interactive |
| Bypass control | Yes (reg 1536) | **No** — no bypass on line-interactive |
| Frequency tolerance | Yes (reg 593, R/W) | **No** — 990-9840 marks this SRT/SURTD only |
| Sensitivity setting | No | **Yes** — reg 1028, SMT only (normal/reduced/low) |
| Outlet groups | MOG + SOG0 + SOG1 | **TBD** — may be MOG only on 750VA |
| Outlet commands (reg 1538) | Rejected over serial RTU | **TBD** |

---

## Register Blocks (from 990-9840, pending verification)

| Block | Start | Length | Purpose | Access | Verified |
|-------|-------|--------|---------|--------|----------|
| Status | 0 | 27 | UPS status, transfer reason, outlet states, signaling | Read | No |
| Dynamic | 128 | 44 | Battery, input, output measurements, timers | Read | No |
| Inventory | 516 | 80+ | Model, serial, firmware, ratings, names | Read (some R/W) | No |
| Settings | 1024-1073 | varies | Test interval, transfer voltages, sensitivity, delays | Read/Write | No |
| Commands | 1536-1543 | varies | UPS commands, test commands | Write | No |

---

## Status Block (Register 0-26)

Read as: `modbus_read_registers(ctx, 0, 27, regs)`

### Registers 0-1: UPSStatus_BF (uint32)

Same bit definitions as SRT per 990-9840. **Bit 13 (HE mode) is not expected to be set** on a line-interactive unit. Bypass-related bits (3, 10) also unlikely.

| Bit | Mask | Meaning | Expected on SMT |
|-----|------|---------|-----------------|
| 1 | 0x00000002 | Online (OL) | Yes |
| 2 | 0x00000004 | On Battery (OB) | Yes |
| 3 | 0x00000008 | Output on Bypass | No |
| 4 | 0x00000010 | Output Off | Yes |
| 5 | 0x00000020 | General Fault | Yes |
| 6 | 0x00000040 | Input Not Acceptable | Yes |
| 7 | 0x00000080 | Self Test in Progress | Yes |
| 8 | 0x00000100 | Pending Output On | Yes |
| 9 | 0x00000200 | Shutdown Pending | Yes |
| 13 | 0x00002000 | High Efficiency Mode | No |
| 15 | 0x00008000 | Fault State | Yes |
| 21 | 0x00200000 | Overload | TBD (990-9840 marks SRT only) |

### Register 2: UPSStatusChangeCause_EN (uint16 enum)

Same values as SRT (0-30). See SRT reference for full table.

### Registers 3-14: Outlet Group Status

**TBD** — SMT750 may only have MOG (no switched outlet groups). Check reg 590 (SOGRelayConfigSetting_BF) to confirm.

### Register 18: SimpleSignalingStatus_BF (uint16 bitfield)

| Bit | Meaning |
|-----|---------|
| 0 | PowerFailure (on battery, output on or off) |
| 1 | ShutdownImminent (UPS committed to disconnecting outputs) |

### Register 19: GeneralError_BF (uint16 bitfield)

| Bit | Meaning |
|-----|---------|
| 0 | SiteWiring |
| 1 | EEPROM |
| 2 | ADConverter |
| 3 | LogicPowerSupply |
| 4 | InternalCommunication |
| 5 | UIButton |
| 7 | EPOActive |
| 8 | FirmwareMismatch |

### Registers 20-21: PowerSystemError_BF (uint32 bitfield)

Constructed as: `(regs[20] << 16) | regs[21]`

| Bit | Meaning |
|-----|---------|
| 0 | OutputOverload |
| 1 | OutputShortCircuit |
| 2 | OutputOvervoltage |
| 4 | Overtemperature |
| 10 | Fan |
| 13 | Inverter |

### Register 22: BatterySystemError_BF (uint16 bitfield)

| Bit | Meaning |
|-----|---------|
| 0 | Disconnected |
| 1 | Overvoltage |
| 2 | NeedsReplacement |
| 3 | OvertemperatureCritical |
| 4 | Charger |
| 5 | TemperatureSensor |
| 7 | OvertemperatureWarning |
| 8 | GeneralError |
| 9 | Communication |

### Register 23: ReplaceBatteryTestStatus_BF

Same as SRT. See SRT reference for full bit layout.

### Register 24: RunTimeCalibrationStatus_BF

Same as SRT. See SRT reference for full bit layout.

### Register 25: Battery.LifeTimeStatus_BF

| Bit | Meaning |
|-----|---------|
| 0 | LifeTimeStatusOK |
| 1 | LifeTimeNearEnd |
| 2 | LifeTimeExceeded |

### Register 26: UserInterfaceStatus_BF

| Bit | Meaning |
|-----|---------|
| 0 | ContinuousTestInProgress |
| 1 | AudibleAlarmInProgress |
| 2 | AudibleAlarmMuted |
| 3 | AnyButtonPressedRecently |

---

## Dynamic Block (Register 128-171)

Read as: `modbus_read_registers(ctx, 128, 44, regs)`

All scaling factors are identical to SRT per 990-9840.

### Battery

| Reg | Offset | Type | Scale | Unit |
|-----|--------|------|-------|------|
| 128-129 | 0-1 | uint32 | raw | Seconds (runtime) |
| 130 | 2 | uint16 | /512 | Percent (charge) |
| 131 | 3 | int16 | /32 | Volts DC (battery positive voltage) |
| 133 | 5 | uint16 | raw | Days since 2000-01-01 (battery date) |
| 135 | 7 | int16 | /128 | Degrees C (temperature) |

### Output

| Reg | Offset | Type | Scale | Unit |
|-----|--------|------|-------|------|
| 136 | 8 | uint16 | /256 | Percent (load / real power) |
| 138 | 10 | uint16 | /256 | Percent (apparent power) |
| 140 | 12 | uint16 | /32 | Amps (current) |
| 142 | 14 | uint16 | /64 | Volts AC (voltage) |
| 144 | 16 | uint16 | /128 | Hz (frequency) |
| 145-146 | 17-18 | uint32 | raw | Wh (energy) |

### Input

| Reg | Offset | Type | Scale | Unit |
|-----|--------|------|-------|------|
| 150 | 22 | uint16 | bitfield | Input.InputStatus_BF (see SRT reference for bit layout) |
| 151 | 23 | uint16 | /64 | Volts AC (voltage) |

### UPS

| Reg | Offset | Type | Scale | Unit |
|-----|--------|------|-------|------|
| 154 | 26 | int16 | /128 | Percent (efficiency, see SRT reference for special negative values) |

---

## Inventory Block (Register 516-635)

Read once at startup: `modbus_read_registers(ctx, 516, 80, regs)`

| Reg | Offset | Length | Variable | Type |
|-----|--------|--------|----------|------|
| 516-523 | 0-7 | 8 | Firmware Version | string (16 chars) |
| 532-547 | 16-31 | 16 | Model Name | string (32 chars) |
| 548-563 | 32-47 | 16 | SKU | string (32 chars) |
| 564-571 | 48-55 | 8 | Serial Number | string (16 chars) |
| 572-579 | 56-63 | 8 | Battery SKU | string (16 chars) |
| 588 | 72 | 1 | Nominal Apparent Power | uint16 (VA) |
| 589 | 73 | 1 | Nominal Real Power | uint16 (Watts) |
| 590 | 74 | 1 | SOGRelayConfigSetting_BF | uint16 BF |
| 591 | 75 | 1 | Manufacture Date | uint16 (days since 2000-01-01) |
| 592 | 76 | 1 | Output.VoltageACSetting_BF | uint16 BF (read-only) |
| 595 | 79 | 1 | Battery Install Date | uint16 (days since 2000-01-01, R/W) |
| 596-603 | 80-87 | 8 | UPS Name | string (16 chars, R/W) |
| 604-611 | 88-95 | 8 | MOG Name | string (16 chars, R/W) |

**Note:** Register 593 (Output.AcceptableFrequencySetting_BF) is **not supported on SMT** per 990-9840 — only SRT and SURTD.

---

## Settings Block (Register 1024-1073)

### Battery Test Interval (Register 1024)

`BatteryTestIntervalSetting_BF` — controls automatic battery self-test schedule.

| Bit | Setting |
|-----|---------|
| 0 | Never |
| 1 | OnStartUpOnly |
| 2 | OnStartUpPlus7 (startup + every 7 days) |
| 3 | OnStartUpPlus14 (startup + every 14 days) |
| 4 | OnStartUp7Since (every 7 days since last test) |
| 5 | OnStartUp14Since (every 14 days since last test) |

### Transfer Voltages

| Reg | Variable | Unit |
|-----|----------|------|
| 1026 | Input Transfer High | Volts |
| 1027 | Input Transfer Low | Volts |

### Sensitivity Setting (Register 1028, SMT only)

`Output.SensitivitySetting_BF` — controls how much input variation is passed through to the load. **Not available on SRT.**

| Bit | Setting |
|-----|---------|
| 0 | Normal (minimum input deviations seen by load) |
| 1 | Reduced (more input deviations seen by load) |
| 2 | Low (maximum input deviations seen by load) |

### Outlet Delays

**TBD** — depends on outlet group availability (reg 590).

---

## Command Registers (Write-Only)

### Register 1536-1537: UPSCommand_BF (uint32, FC16)

| Bit | Mask | Command | Expected on SMT |
|-----|------|---------|-----------------|
| 3 | 0x00000008 | Restore Factory Settings | Yes (do not test) |
| 9 | 0x00000200 | Clear Faults | Yes |
| 13 | 0x00002000 | Reset Strings | Yes |
| 14 | 0x00004000 | Reset Logs | Yes |

Bypass commands (bits 4, 5) are not expected to work on line-interactive SMT.

### Register 1538-1539: OutletCommand_BF (uint32, FC16)

See SRT reference for full bit layout. **TBD** whether this works on SMT over serial RTU.

### Register 1540: SimpleSignalingCommand_BF (uint16, FC06)

Expected to work the same as SRT:

| Bit | Command |
|-----|---------|
| 0 | RequestShutdown |
| 1 | RemoteOff (equivalent to holding power off button) |
| 2 | RemoteOn (equivalent to pressing ON power button) |

### Register 1541: ReplaceBatteryTestCommand_BF (uint16, FC06)

| Bit | Command |
|-----|---------|
| 0 | Start Battery Test |
| 1 | Abort Battery Test |

### Register 1542: RunTimeCalibrationCommand_BF (uint16, FC06)

| Bit | Command |
|-----|---------|
| 0 | Start Runtime Calibration |
| 1 | Abort Runtime Calibration |

### Register 1543: UserInterfaceCommand_BF (uint16, FC06)

| Bit | Command |
|-----|---------|
| 0 | ShortTest (momentary LED/beeper test) |
| 1 | ContinuousTest |
| 2 | MuteAllActiveAudibleAlarms |
| 3 | CancelMute |
| 5 | AcknowledgeBatteryAlarms |
| 6 | AcknowledgeSiteWiringAlarm |

---

## Hardware Notes

- **Model:** APC SMT750RM2U (Smart-UPS SMT 750VA / 500W, 2U rack-mount)
- **Serial port:** RJ50 (10P10C), requires **940-0625A cable** (RJ50 to DB9)
- **Firmware:** TBD
- **USB Modbus:** Reportedly unreliable on SMT series — use serial with genuine FTDI adapter

---

## Testing Checklist

When the SMT750RM2U arrives, verify the following:

- [ ] Modbus communication over serial RTU (9600 8N1)
- [ ] Status block read (reg 0, 27 regs) — all fields including GeneralError (19), PowerSystemError (20-21), BatterySystemError (22), LifeTimeStatus (25), UIStatus (26)
- [ ] Dynamic block read (reg 128, 44 regs)
- [ ] Inventory block read (reg 516, 80 regs)
- [ ] Outlet group configuration (reg 590)
- [ ] Transfer thresholds (reg 1026-1027)
- [ ] Sensitivity setting (reg 1028) — verify R/W, test all 3 values
- [ ] Shutdown command (reg 1540 = 0x0001)
- [ ] Battery test command (reg 1541 = 0x0001)
- [ ] Mute/unmute commands (reg 1543)
- [ ] Beep test (reg 1543 = 0x0001)
- [ ] Clear faults (reg 1536-1537)
- [ ] Whether bypass commands work (reg 1536 bits 4-5)
- [ ] Whether HE/ECO status bit 13 is ever set
- [ ] Whether frequency tolerance register (593) is readable/writable
- [ ] Battery test interval setting (reg 1024)
- [ ] Protocol verification block (reg 2048-2061) — test values
- [ ] Register scan for any SMT-specific registers not in the SRT map

---

## References

- APC Register Map 990-9840B (SPD_LFLG-A32G3L_EN) — covers SMT, SMX, SURTD, and SRT
- APC Application Note #176 (MPAO-98KJ7F_EN)
- APC SRT Modbus Reference (../APC_SRT_MODBUS_REFERENCE.md) — verified register map
