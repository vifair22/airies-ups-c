# APC Smart-UPS SMT Modbus Reference

Findings from direct testing against an **APC SMT1500RM2UC** (FW: `UPS 04.1`, ModbusMapID `00.5`, SKU `SMT1500RM2UC`, S/N `3S2042X14540`) over **serial RTU** (`/dev/ttyUSB0`, 9600 8N1, slave 1) using `libmodbus`.

This document is the canonical reference for the in-tree `ups_smt` driver. It is built from APC's published materials — **Application Note #176** (Modbus Implementation in APC Smart-UPS) and **Register Map 990-9840B** (Smart-UPS models with prefix SMT/SMX/SURTD/SRT) — cross-checked against live hardware. Where the wire-level behaviour differs from what either document implies, that difference is called out as a **Quirk** so future maintainers don't waste cycles relearning it.

The companion SRT reference (`apc-srt-modbus.md`) covers a different model in the same family. The protocol framing is identical; the differences are in which fields populate, which commands the firmware accepts, and the topology-driven capability set.

---

## Firmware version coverage

> **Verified baseline: `UPS 04.1`, `ModbusMapID 00.5` on SMT1500RM2UC.** This is roughly a decade old. Current shipping SMT firmware is `UPS 18.x` (2024/2025 vintage). The categories below are the things most likely to drift between firmware generations — if you re-run the probe against a newer unit, update the cells with the observed value (or a check mark + the firmware version) so the doc continues to reflect reality.

| Category | Verified on FW 04.1 | Most-recent firmware seen | Notes |
|----------|---------------------|---------------------------|-------|
| `ModbusMapID` (reg 2048) | `"00.5"` | TBD | Not load-bearing for the driver — but the value is part of the protocol-verification block, so a bump usually signals broader changes downstream. |
| `UPSStatusChangeCause_EN` (reg 2) | values 0–30 per AN-176 | TBD | APC may have added new transition causes; unknown values pass through untouched as integers. |
| Status bit 13 (HighEfficiency) | observed set on idle | TBD | Newer firmware may split HE into sub-modes that this single bit can no longer distinguish. |
| `SOGRelayConfigSetting_BF` (reg 590) | `0x0003` (MOG + SOG0) on this SKU | TBD per SKU | Larger SMTs (SMT3000, SMTLxxxRM) may light up SOG1/SOG2/SOG3. The driver's `resolve_config_regs` already adapts. |
| `Output.SensitivitySetting_BF` (reg 1028) | bits 0/1/2 all writable (Normal/Reduced/Low) — verified by probe | TBD | If APC has added a fourth setting, the driver's bitfield options need a new entry. |
| `Output.VoltageACSetting_BF` short form (reg 592) | bits 0–6 + bit 11 | TBD | The 32-bit long form at reg 644 already lists VAC100_200, VAC115, VAC125 and split-phase auto-select; newer firmware may rely on those instead. |
| SKU naming pattern | `SMT1500RM2UC` matches `"SMT"` substring | assume stable | If APC ever ships a newer line with a non-"SMT" prefix on the SKU, the driver's detect needs to widen. |
| `BatteryTestIntervalSetting_BF` (reg 1024) | bits 0/1/4/5 accepted, bits 2/3 firmware-rejected — verified by probe | TBD | Same constraint as the SRT line. Driver omits bits 2/3 from `smt_bat_test_opts`. |
| Bypass-related registers (147, 148, 149) | `0x0000` / `0xFFFF` (N/A) | assume stable | Bypass is fundamentally not a feature of line-interactive topology, so a firmware update is unlikely to change this. |
| `Output.AcceptableFrequencySetting_BF` (reg 593) | `0x0000` (N/A) | assume stable | Per 990-9840B this register is SRT/SURTD-only; same reasoning as bypass. |
| Timing/runtime scalar range (regs 1029, 1030, 1033, 1034, 1035, 1038) | 0..32767 enforced (writes >= 32768 return modbus exc 0x04) — verified by probe | TBD | Spec says UINT16 (0..65535); firmware enforces signed int16 range. Driver descriptors set `meta.scalar.max = 32767` to match. |
| Load Shed registers (regs 1054, 1056, 1064, 1068, 1072, 1073) | **Not implemented for write** — Illegal Data Address on every write attempt; reads return AN-176 "not applicable" sentinels (`0x0000` / `0xFFFF`) — verified by probe | TBD | Driver flips `writable=0` on these descriptors. Re-verify on FW 18.x — newer firmware may implement load shed and possibly use 65535 as a "disabled" sentinel on the threshold registers. See "Implementation gaps on FW UPS 04.1" section below for full details. |

**When you re-probe against a newer unit:**

1. Run a fresh capture of registers 0, 128, 516, 1024, and 2048 (status / dynamic / inventory / settings / protocol-verification blocks).
2. Diff against the live values quoted throughout this document.
3. For each row above, fill in the "Most-recent firmware seen" column with the observed value or "matches FW 04.1".
4. If a Quirk callout is contradicted (e.g. newer firmware actually sets bit 10 of UPSStatus on SMT, or returns a non-zero value at reg 593), promote the contradiction to a new Quirk and reference both firmware versions so the change is visible in history.

---

## Protocol

| Parameter | Value | Source |
|-----------|-------|--------|
| Transport | Modbus RTU over RS-232 (USB-to-serial adapter) | AN-176 §4.3 |
| Baud rate | 9600 (fixed on SMT) | AN-176 §4.3 |
| Data bits | 8 | AN-176 §4.3 |
| Parity | **None** (NOT Even — APC departs from spec) | AN-176 §4.3 |
| Stop bits | 1 | AN-176 §4.3 |
| Slave ID | 1 by default; configurable 1–223 (224–247 reserved) | AN-176 §4.1 |
| Function codes | FC03 (Read Holding Registers), FC06 (Write Single), FC16 (Write Multiple) | AN-176 §5 |
| Inter-frame gap | 35 ms minimum | AN-176 §4.2.2 |
| Inter-character timeout | 15 ms minimum | AN-176 §4.2.2 |
| Response timeout | 250 ms minimum | AN-176 §4.2.2 |
| Byte order | Big-endian; 32-bit values are MSB register first | 990-9840B note 1 |
| Signed numbers | Two's complement | 990-9840B note 7 |

**Modbus must be enabled on the UPS first** — front panel → Advanced Menu → Configuration → Enable Modbus. APC ships with it disabled (AN-176 §6.3).

### BPI scaling notation

APC encodes most numeric measurements as **Binary-Point Integers** with a fixed `2^bpl` divisor. The 990-9840B map's "Scale (Divide Reading By)" column gives the divisor directly. Examples used by SMT:

| Code | Divisor | Used for |
|------|---------|----------|
| U7 / S7 | /128 | Output frequency, battery temperature |
| U6 | /64 | Output voltage AC, input voltage AC |
| U5 | /32 | Output current AC, battery voltage DC |
| U8 | /256 | Real / apparent power percent |
| U9 | /512 | State of charge percent |
| U0 / S0 | /1 | Watt-hours, runtime seconds, countdown timers |

### String fields

ASCII range 0x20–0x7E, big-endian within each register (high byte first). **Strings are not NUL-terminated.** Padding is officially 0x20 (space) per AN-176 §1.3.3, but the SMT1500RM2UC was observed mixing 0x00 and 0x20 padding in the same field. The driver's string decoder must strip both leading 0x00 and trailing 0x20.

---

## Register Blocks

| Block | Start | Length | Purpose | Access |
|-------|-------|--------|---------|--------|
| Status | 0 | 27 | Status bits, transfer reason, outlet states, errors | Read |
| Dynamic | 128 | 44 | Battery, input, output measurements, timers | Read |
| Inventory | 516 | 80 | Firmware, model, SKU, serial, ratings, SOG config | Read (once at connect) |
| Names | 596 | 40 | UPS / MOG / SOG names | Read/Write strings |
| Settings | 1024 | ~50 | Test interval, transfer thresholds, sensitivity, outlet timing, load shed | Read/Write |
| Commands | 1536–1543 | 8 | UPS / outlet / signaling / test / UI commands | Write |
| Protocol Verification | 2048 | 14 | Fixed test values to validate byte order and scaling | Read |

Bulk reads of an entire block succeed even when individual registers within are reserved or not applicable; the inapplicable cells return `0x0000` or `0xFFFF` (see Quirks).

---

## Status Block (registers 0–26)

Read as: `modbus_read_registers(ctx, 0, 27, regs)`

### Registers 0–1 — `UPSStatus_BF` (uint32)

Constructed as: `(regs[0] << 16) | regs[1]`. Bit indices below match the AN-176 §B layout. The "SMT" column is what the live SMT1500RM2UC actually toggles.

| Bit | Mask | Meaning | SMT |
|-----|------|---------|-----|
| 1 | 0x00000002 | StateOnline | yes |
| 2 | 0x00000004 | StateOnBattery | yes |
| 3 | 0x00000008 | StateBypass | **no** (line-interactive — no bypass path) |
| 4 | 0x00000010 | StateOutputOff | yes |
| 5 | 0x00000020 | Fault-Modifier | yes |
| 6 | 0x00000040 | InputBad-Modifier | yes |
| 7 | 0x00000080 | Test-Modifier | yes |
| 8 | 0x00000100 | PendingOutputOn-Modifier | yes |
| 9 | 0x00000200 | PendingOutputOff-Modifier | yes |
| 10 | 0x00000400 | Commanded (manual bypass via firmware) | **no** on SMT |
| 13 | 0x00002000 | HighEfficiency-Modifier (ECO/green mode) | **yes** (observed `0x00002022` in service) |
| 14 | 0x00004000 | InformationalAlert-Modifier | yes |
| 15 | 0x00008000 | FaultState-Modifier | yes |

> **Quirk — SMT reports HighEfficiency mode.** AN-176 marks bit 13 as supported on SMX/SMT in 990-9840B and the live SMT1500RM2UC sets it. Driver should expose `UPS_CAP_HE_MODE`.

> **Quirk — bit 5 (Fault) can be sticky.** During a healthy idle observation the unit reported `0x00002022` (Online + Fault + HE) with `PowerSystemError = 0` and only `BatterySystemError bit 0` (Disconnected) set. The Fault flag mirrors latched battery conditions, not the live power path.

### Register 2 — `UPSStatusChangeCause_EN` (uint16 enum)

Same enum as the rest of the family (0–30, see AN-176 Appendix C / 990-9840B). Only the values that can be triggered by an SMT's transitions actually appear on this product, but the enum itself is full-width.

### Registers 3–14 — Outlet Group Status

| Reg | Field | Length | Notes |
|-----|-------|--------|-------|
| 3–4 | `MOG.OutletStatus_BF` | 2 | Always present on SMT |
| 5 | reserved | 1 | reads `0x0000` |
| 6–7 | `SOG[0].OutletStatus_BF` | 2 | Present on the units that have SOG0 (see reg 590) |
| 8 | reserved | 1 | |
| 9–10 | `SOG[1].OutletStatus_BF` | 2 | Some SMT skus only |
| 11 | reserved | 1 | |
| 12–13 | `SOG[2].OutletStatus_BF` | 2 | Some SMT skus only |
| 14 | reserved | 1 | |

Bit layout (per group, identical to AN-176 OutletStatus_BF):

| Bit | Meaning |
|-----|---------|
| 0 | StateOn (mutually exclusive with StateOff) |
| 1 | StateOff |
| 2 | ProcessReboot in progress |
| 3 | ProcessShutdown in progress |
| 4 | ProcessSleep in progress |
| 7 | PendingOffDelay |
| 8 | PendingOnACPresence |
| 9 | PendingOnMinRuntime |
| 10 | MemberGroupProcess1 |
| 11 | MemberGroupProcess2 |
| 12 | LowRuntime |

> **Quirk — actual SOG count varies per SKU.** On the SMT1500RM2UC, register 590 returned `0x0003` = MOG + SOG0 only. Reads of SOG1/SOG2 status return zeros (no exception), so the bit-meaningful "this group exists" signal is reg 590, not the absence of an exception on the status read. The driver should use `resolve_config_regs` to drop SOG-specific descriptors that don't exist on the live unit.

### Registers 15–17 — Reserved

Read as zero, including in bulk.

### Register 18 — `SimpleSignalingStatus_BF`

| Bit | Meaning |
|-----|---------|
| 0 | PowerFailure (input not acceptable) |
| 1 | ShutdownImminent (UPS committed to dropping output) |

### Register 19 — `GeneralError_BF`

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

(990-9840B defines additional bits 6, 9–15 for SRT/SURTD only.)

### Registers 20–21 — `PowerSystemError_BF` (uint32)

Constructed as: `(regs[20] << 16) | regs[21]`.

| Bit | Meaning |
|-----|---------|
| 0 | OutputOverload |
| 1 | OutputShortCircuit |
| 2 | OutputOvervoltage |
| 4 | Overtemperature |
| 5 | BackfeedRelay fault |
| 6 | AVRRelay fault |
| 7 | PFCInputRelay fault |
| 8 | OutputRelay fault |
| 10 | Fan fault |
| 11 | PFC fault |
| 13 | Inverter fault |

### Register 22 — `BatterySystemError_BF`

| Bit | Meaning |
|-----|---------|
| 0 | Disconnected |
| 1 | Overvoltage |
| 2 | NeedsReplacement |
| 3 | OvertemperatureCritical |
| 4 | Charger fault |
| 5 | TemperatureSensor fault |
| 7 | OvertemperatureWarning |
| 8 | GeneralError |
| 9 | Communication |

### Register 23 — `ReplaceBatteryTestStatus_BF` (sticky)

Latched results of the most recent battery self-test. Bits 0–5 encode the result modifier (Pending / InProgress / Passed / Failed / Refused / Aborted), bits 6–8 are the source modifier (Protocol / LocalUI / Internal), bits 9–11 are the result modifier (InvalidState / InternalFault / StateOfChargeNotAcceptable). A value of `0x0310` was observed at startup, indicating a prior `Refused` test from the `Internal` source due to `InvalidState` — typical for a brand-new install.

### Register 24 — `RunTimeCalibrationStatus_BF` (sticky)

Same encoding shape as register 23 (Pending/InProgress/Passed/Failed/Refused/Aborted + source + result modifier). Bits 12–15 add LoadChange / ACInputNotAcceptable / LoadTooLow / OverChargeInProgress.

### Register 25 — `Battery.LifeTimeStatus_BF`

| Bit | Meaning |
|-----|---------|
| 0 | LifeTimeStatusOK |
| 1 | LifeTimeNearEnd |
| 2 | LifeTimeExceeded |
| 3 | LifeTimeNearEndAcknowledged |
| 4 | LifeTimeExceededAcknowledged |

### Register 26 — `UserInterfaceStatus_BF`

| Bit | Meaning |
|-----|---------|
| 0 | ContinuousTestInProgress |
| 1 | AudibleAlarmInProgress |
| 2 | AudibleAlarmMuted |
| 3 | AnyButtonPressedRecently |

---

## Dynamic Block (registers 128–171)

Read as: `modbus_read_registers(ctx, 128, 44, regs)`

### Battery

| Reg | Offset | Type | Scale | Field | Notes |
|-----|--------|------|-------|-------|-------|
| 128–129 | 0–1 | uint32 | /1 | RunTimeRemaining | Seconds. Max 65535. Observed `0x00015180` = 86400 (1 day) on idle SMT — interpret as "saturated, very high". |
| 130 | 2 | uint16 | /512 | StateOfCharge_Pct | 0–100% |
| 131 | 3 | int16 | /32 | Battery.Positive.VoltageDC | Single-bus DC voltage |
| 132 | 4 | int16 | /32 | Battery.Negative.VoltageDC | **N/A on SMT** (returns `0xFFFF`) |
| 133 | 5 | uint16 | /1 | Battery.Date | Theoretical replacement date, days since 2000-01-01 |
| 134 | 6 | reserved | — | — | |
| 135 | 7 | int16 | /128 | Battery.Temperature | Degrees C |

### Output (single-phase only on SMT)

| Reg | Offset | Type | Scale | Field |
|-----|--------|------|-------|-------|
| 136 | 8 | uint16 | /256 | Output[0].RealPower_Pct |
| 138 | 10 | uint16 | /256 | Output[0].ApparentPower_Pct |
| 140 | 12 | uint16 | /32 | Output[0].CurrentAC (amperes) |
| 142 | 14 | uint16 | /64 | Output[0].VoltageAC (volts) |
| 144 | 16 | uint16 | /128 | Output.Frequency (Hz) |
| 145–146 | 17–18 | uint32 | /1 | Output.Energy (watt-hours, lifetime) |

Registers 137, 139, 141, 143 are SURTD second-phase fields and are not applicable on SMT.

### Bypass — not applicable on SMT

| Reg | Offset | Field | SMT behaviour |
|-----|--------|-------|---------------|
| 147 | 19 | Bypass.InputStatus_BF | reads `0x0000` |
| 148 | 20 | Bypass.VoltageAC | reads `0xFFFF` (would scale to 1023.984 V — sentinel, not data) |
| 149 | 21 | Bypass.Frequency | reads `0xFFFF` (sentinel) |

> **Quirk — bypass values must be ignored.** The driver must NOT populate `ups_data.bypass_*` fields on SMT; the registry then leaves them zero, which downstream consumers treat as "not present".

### Input

| Reg | Offset | Type | Scale | Field |
|-----|--------|------|-------|-------|
| 150 | 22 | uint16 | bitfield | Input.InputStatus_BF (see SRT reference for bit layout) |
| 151 | 23 | uint16 | /64 | Input[0].VoltageAC |

### Efficiency

| Reg | Offset | Type | Scale | Field |
|-----|--------|------|-------|-------|
| 154 | 26 | int16 | mixed | Efficiency_EN |

Encoding (same as 990-9840B for the whole family):

| Raw | Meaning |
|-----|---------|
| `>= 0` | `raw / 128` = efficiency percent |
| -1 | NotAvailable |
| -2 | LoadTooLow (observed on idle SMT with no load) |
| -3 | OutputOff |
| -4 | OnBattery |
| -5 | InBypass |
| -6 | BatteryCharging |
| -7 | PoorACInput |
| -8 | BatteryDisconnected |

The driver splits this into `ups_data.efficiency_reason` (the enum) and `ups_data.efficiency` (a percent valid only when reason is `UPS_EFF_OK`).

### Outlet group countdowns

| Reg | Offset | Type | Field |
|-----|--------|------|-------|
| 155 | 27 | int16 | MOG.TurnOffCountdown_EN |
| 156 | 28 | int16 | MOG.TurnOnCountdown_EN |
| 157–158 | 29–30 | int32 | MOG.StayOffCountdown_EN |
| 159 | 31 | int16 | SOG[0].TurnOffCountdown_EN |
| 160 | 32 | int16 | SOG[0].TurnOnCountdown_EN |
| 161–162 | 33–34 | int32 | SOG[0].StayOffCountdown_EN |
| 163 | 35 | int16 | SOG[1].TurnOffCountdown_EN |
| 164 | 36 | int16 | SOG[1].TurnOnCountdown_EN |
| 165–166 | 37–38 | int32 | SOG[1].StayOffCountdown_EN |
| 167 | 39 | int16 | SOG[2].TurnOffCountdown_EN |
| 168 | 40 | int16 | SOG[2].TurnOnCountdown_EN |
| 169–170 | 41–42 | int32 | SOG[2].StayOffCountdown_EN |

Sentinel `-1` means "no countdown active".

> **Quirk — registers above the device's actual SOG count return `0xFFFF`.** On the MOG+SOG0-only SMT1500RM2UC, regs 163+ all read `0xFFFF`. This decodes harmlessly as `-1` (NotActive) for the int16 fields and as a very negative number for the int32 fields, so consumers see "no countdown active" — but a driver that exposes the SOG2 timer to the UI on a unit without SOG2 would mislead the operator. Use `resolve_config_regs` to drop them at connect time.

---

## Inventory Block (registers 516–595, 80 registers)

Read once at connect: `modbus_read_registers(ctx, 516, 80, regs)`

| Reg | Offset | Length (regs) | Type | Field | Live value |
|-----|--------|---------------|------|-------|-----------|
| 516–523 | 0–7 | 8 | string (16 chars) | `FWVersion_STR` | `"UPS 04.1"` |
| 524–531 | 8–15 | 8 | reserved | reads `0xFFFF` | |
| 532–547 | 16–31 | 16 | string (32 chars) | `Model_STR` | `"Smart-UPS 1500"` |
| 548–563 | 32–47 | 16 | string (32 chars) | `SKU_STR` | `"SMT1500RM2UC"` |
| 564–571 | 48–55 | 8 | string (16 chars) | `SerialNumber_STR` | `"3S2042X14540"` |
| 572–579 | 56–63 | 8 | string (16 chars) | `Battery.SKU_STR` | `"APCRBC133"` |
| 580–587 | 64–71 | 8 | string | `Battery.ExternalBattery.SKU_STR` (SRT only) | reads zeros on SMT |
| 588 | 72 | 1 | uint16 | `Output.ApparentPowerRating` (VA) | `1440` |
| 589 | 73 | 1 | uint16 | `Output.RealPowerRating` (W) | `1000` |
| 590 | 74 | 1 | uint16 BF | `SOGRelayConfigSetting_BF` | `0x0003` |
| 591 | 75 | 1 | uint16 | `Manufacture.Date` (days since 2000-01-01) | `7594` |
| 592 | 76 | 1 | uint16 BF | `Output.VoltageACSetting_BF` (RO short form) | `0x0002` (VAC120) |
| 593 | 77 | 1 | uint16 BF | `Output.AcceptableFrequencySetting_BF` | **`0x0000` on SMT (defined-but-inapplicable)** |
| 594 | 78 | 1 | reserved | | |
| 595 | 79 | 1 | uint16 RW | `Battery.DateSetting` (install date, days since 2000-01-01) | `7684` |

> **Quirk — driver must detect via SKU, not Model.** The 1500 VA SMT presents `Model_STR = "Smart-UPS 1500"` with no family prefix. Detection must read register 548 (SKU_STR) and substring-match `"SMT"`. The SRT driver gets away with reading Model_STR because SRT models include "SRT" in the model string; the SMT line does not.

> **Quirk — apparent VA rating is not a round number.** Live SMT1500 reports 1440 VA (not 1500). Use this field as-is; do not "round to nearest 100" or compare to the SKU number.

> **Quirk — register 593 returns `0x0000` on SMT, not `0xFFFF`.** AN-176 §3.2.1 says undefined registers return `0xFFFF`, but AcceptableFrequencySetting is *defined* family-wide and returns its (zero-bits) bitfield value on SMT because it's "not applicable" rather than "undefined". The driver must not advertise `UPS_CAP_FREQ_TOLERANCE` for SMT regardless.

### Names — separate read at registers 596–635

| Reg | Length (regs) | Field | Live value | Writable |
|-----|---------------|-------|------------|----------|
| 596–603 | 8 | `Name_STR` (UPS) | `"APCUPS"` | yes |
| 604–611 | 8 | `MOG.Name_STR` | `"UPS Outlets"` | yes |
| 612–619 | 8 | `SOG[0].Name_STR` | `"Outlet Group 1"` | yes |
| 620–627 | 8 | `SOG[1].Name_STR` | empty | yes (per spec) |
| 628–635 | 8 | `SOG[2].Name_STR` | empty | yes (per spec) |

These are 16-character strings (8 registers × 2 chars). Padding mixes 0x20 and 0x00. Writes must be left-justified, padded with 0x20, no NUL terminator.

### Long-form `Output.VoltageACSetting_BF` (register 644, 32-bit RO)

The 32-bit form at register 644–645 lists the same voltage options plus extras (VAC100_200, VAC115, VAC125, plus split-phase auto-selection bits). On the SMT1500RM2UC the short form at register 592 was sufficient; the long form is exposed by the driver as a read-only descriptor for completeness.

---

## Settings Block (registers 1024–1073)

### Battery test interval — register 1024 (`BatteryTestIntervalSetting_BF`, RW)

| Value | Bit | Setting | Status |
|-------|-----|---------|--------|
| 1 | 0 | Never | Accepted |
| 2 | 1 | OnStartUpOnly | Accepted |
| 4 | 2 | OnStartUpPlus7 (startup + every 7 days) | **Defined in spec, rejected on SMT FW UPS 04.1** |
| 8 | 3 | OnStartUpPlus14 | **Defined in spec, rejected on SMT FW UPS 04.1** |
| 16 | 4 | OnStartUp7Since (every 7 days since last test, observed default `0x10`) | Accepted |
| 32 | 5 | OnStartUp14Since | Accepted |

Live value `0x0010` = OnStartUp7Since.

#### Rejected Values (SMT1500RM2UC FW UPS 04.1)

Values 4 (OnStartUpPlus7) and 8 (OnStartUpPlus14) are defined in AN-176 / 990-9840B but rejected by this firmware with Modbus exception 0x04 ("Slave device or server failure"). The same constraint exists on the SRT line per `apc-srt-modbus.md` — the bits appear to be spec-defined but never wired into operational firmware. The driver omits them from `smt_bat_test_opts` and the registry's strict-bitfield validation blocks writes of these values before they hit the wire.

| Value | Result |
|-------|--------|
| 4, 8 | Slave device or server failure (Modbus exception 0x04) |

### Transfer voltages — registers 1026 / 1027 (RW)

| Reg | Field | Live | Notes |
|-----|-------|------|-------|
| 1026 | `Output.UpperAcceptableVoltageSetting` | `127 V` | Volts directly, no scaling |
| 1027 | `Output.LowerAcceptableVoltageSetting` | `106 V` | Volts directly, no scaling |

Driver returns these as volts to the daemon's `ups_read_thresholds` contract.

### Sensitivity — register 1028 (`Output.SensitivitySetting_BF`, RW, **SMT only**)

| Bit | Value | Setting |
|-----|-------|---------|
| 0 | 0x01 | Normal (minimum input deviations seen by load — observed default) |
| 1 | 0x02 | Reduced (more deviations passed through) |
| 2 | 0x04 | Low (maximum deviations passed through) |

> **This descriptor is SMT-specific.** 990-9840B marks this register as SMX/SMT only — neither SRT nor SURTD support it. The SRT driver must not expose it. Conversely, `Output.AcceptableFrequencySetting_BF` (reg 593) is SRT/SURTD only and the SMT driver must not expose it.

### MOG outlet timing — registers 1029–1033 (RW)

| Reg | Type | Field | Live |
|-----|------|-------|------|
| 1029 | int16 | `MOG.TurnOffCountdownSetting_EN` (off delay seconds) | `60` |
| 1030 | int16 | `MOG.TurnOnCountdownSetting_EN` (on delay seconds) | `15` |
| 1031–1032 | int32 | `MOG.StayOffCountdownSetting_4B` (sleep duration) | `15` |
| 1033 | uint16 | `MOG.MinimumReturnRuntimeSetting` | `420` |

### SOG[0/1/2] outlet timing — registers 1034–1048 (RW)

Same per-group layout as MOG but starting at 1034 (SOG0), 1039 (SOG1), 1044 (SOG2) with `MinReturnRuntime` at 1038/1043/1048.

### Load shed configuration — registers 1054–1073 (declared RW, see implementation gap)

| Reg | Field |
|-----|-------|
| 1054 (uint32) | `MOG.LoadShedConfigSetting_BF` |
| 1056 (uint32) | `SOG[0].LoadShedConfigSetting_BF` |
| 1058 (uint32) | `SOG[1].LoadShedConfigSetting_BF` |
| 1060 (uint32) | `SOG[2].LoadShedConfigSetting_BF` |
| 1064 / 1065 / 1066 | `SOG[0..2].LoadShedRunTimeRemainingSetting` |
| 1068 / 1069 / 1070 | `SOG[0..2].LoadShedTimeOnBatterySetting` |
| 1072 | `MOG.LoadShedRunTimeRemainingSetting` |
| 1073 | `MOG.LoadShedTimeOnBatterySetting` |

`LoadShedConfigSetting_BF` bits:

| Bit | Meaning |
|-----|---------|
| 0 | UseOffDelay-Modifier |
| 1 | ManualRestartRequired-Modifier |
| 3 | TimeOnBattery — shed when on battery longer than `LoadShedTimeOnBatterySetting` |
| 4 | RunTimeRemaining — shed when remaining runtime ≤ `LoadShedRunTimeRemainingSetting` |
| 5 | UPSOverload — immediate shed when overloaded (SOG only, not MOG) |

---

## Implementation gaps on FW UPS 04.1

Aggregating the firmware-vs-spec mismatches discovered while bringing up the SMT driver. Each one was reproduced against the SMT1500RM2UC test unit using a libmodbus probe; if you re-test on a newer firmware vintage and the behaviour has changed, please update this section so the driver author has a verified trail.

### Effective scalar ranges differ from spec data type

The 990-9840B map declares the timing/runtime scalar registers (TurnOff/TurnOn delays, MinReturnRuntime) as plain UINT16 with no documented upper bound — implying valid range 0..65535. In practice the firmware enforces a tighter `0..32767` (signed int16 max), and writes of any value `>= 32768` return Modbus exception 0x04 ("Slave device or server failure"). Verified directly on regs 1029, 1033, 1038. Driver descriptors set `meta.scalar = { 0, 32767 }` to match firmware reality; the strict-bitfield validation in `ups_config_write` therefore blocks out-of-range writes before they hit the wire.

### Load Shed registers (1054, 1056, 1064, 1068, 1072, 1073) are not implemented for write

The 990-9840B map declares all six SMX/SMT-applicable load-shed registers as RW. On this firmware, **every write** returns Modbus exception 0x02 ("Illegal Data Address") — the AN-176 §3.2.2 signal that the register is "Not Applicable" on this product. Reads return AN-176's "value for un-implemented register": `0x0000` for the config bitfields (1054, 1056), `0xFFFF` for the threshold scalars (1072, 1073). For the SOG groups, depending on the SKU's actual SOG count, some threshold registers (1064 = SOG0 LoadShedRunTimeRemain, 1068 = SOG0 LoadShedTimeOnBattery) report sane-looking values like 120 / 300 — but those are factory defaults left by the production-line firmware, not user-set values, and writes to them still get Illegal Data Address.

In summary: the entire load-shed feature appears not implemented on FW UPS 04.1. The driver flips these descriptors to `writable=0` so the UI offers no Edit button rather than letting operators try writes that always fail. Re-verify on FW 18.x — if writes are accepted there, flip back to `writable=1` and re-evaluate whether `0xFFFF` functions as a "disabled" sentinel value on the threshold registers.

### Rapid-fire config writes can desync the control plane

Writing to multiple registers in the 1024-1073 block in quick succession (sub-200ms intervals) reliably triggers a control plane reset on this firmware: subsequent reads return Invalid CRC or Illegal Data Address spuriously, and the bus needs ~1-2 seconds to settle before it accepts further traffic. Same quirk documented for the SRT line in `apc-srt-modbus.md` "Rapid-fire config register writes" — the registry's existing 200ms post-write quiet window (see `post_command_settle()` in `src/ups/ups.c`) is the workaround. Drivers should not bypass it.

---

## Command Registers (write-only)

### Register 1536–1537 — `UPSCommand_BF` (uint32, FC16)

| Bit | Mask | Command | SMT |
|-----|------|---------|-----|
| 3 | 0x00000008 | RestoreFactorySettings | yes |
| 4 | 0x00000010 | OutputIntoBypass | **no — line-interactive, no bypass path** |
| 5 | 0x00000020 | OutputOutOfBypass | **no** |
| 9 | 0x00000200 | ClearFaults | yes |
| 13 | 0x00002000 | ResetStrings | yes |
| 14 | 0x00004000 | ResetLogs | yes |

Driver writes 32-bit values via FC16 as `[regs[0]=high, regs[1]=low]`, big-endian.

### Register 1538–1539 — `OutletCommand_BF` (uint32, FC16)

Outlet on/off/reboot/shutdown with delay/source modifiers. Driver does not currently surface this directly; outlet automation is handled via the timing settings instead.

### Register 1540 — `SimpleSignalingCommand_BF` (uint16, FC06)

| Bit | Command |
|-----|---------|
| 0 | RequestShutdown — accepted regardless of UPS state; the master must only issue at the appropriate time |
| 1 | RemoteOff — equivalent to holding the OFF button |
| 2 | RemoteOn — equivalent to pressing ON |

The `shutdown` command in the driver writes `0x0001` to this register.

### Register 1541 — `ReplaceBatteryTestCommand_BF` (uint16, FC06)

| Bit | Command |
|-----|---------|
| 0 | Start battery test |
| 1 | Abort battery test |

### Register 1542 — `RunTimeCalibrationCommand_BF` (uint16, FC06)

| Bit | Command |
|-----|---------|
| 0 | Start runtime calibration |
| 1 | Abort runtime calibration |

The SMT firmware honours runtime calibration. Per `RunTimeCalibrationStatus_BF` (reg 24), the unit will refuse if the load is too low (bit 14, `LoadTooLow`) or the AC input is unacceptable — this is a hardware constraint, not a driver concern.

### Register 1543 — `UserInterfaceCommand_BF` (uint16, FC06)

| Bit | Command |
|-----|---------|
| 0 | ShortTest (momentary LED + beeper) |
| 1 | ContinuousTest (continuous beep + LEDs until cancelled by ShortTest or local mute) |
| 2 | MuteAllActiveAudibleAlarms |
| 3 | CancelMute |
| 5 | AcknowledgeBatteryAlarms |
| 6 | AcknowledgeSiteWiringAlarm |

---

## Protocol Verification (registers 2048–2061)

Read these to confirm wire-level byte order before trusting any other read. Live SMT1500RM2UC matches all expected values:

| Reg | Field | Type | Expected | Observed |
|-----|-------|------|----------|----------|
| 2048–2049 | `ModbusMapID` | string (4 chars) | per device | `"00.5"` |
| 2050–2053 | `TestString` | string (8 chars) | `"12345678"` | `"12345678"` |
| 2054–2055 | `Test4BNumber1` | uint32 | `0x12345678` | `0x12345678` |
| 2056–2057 | `Test4BNumber2` | int32 | `-5` | `-5` |
| 2058 | `Test2BNumber1` | uint16 | `0x1234` | `0x1234` |
| 2059 | `Test2BNumber2` | int16 | `-5` | `-5` |
| 2060 | `TestBPINumber1` | int16 (BPI S6) | `+128.5` | `+128.5` |
| 2061 | `TestBPINumber2` | int16 (BPI S6) | `-128.5` | `-128.5` |

The driver does not read these blocks at runtime, but they are the first thing to consult when integrating against a new SMT model — if any value is off, byte order or scaling assumptions are wrong everywhere.

---

## Driver capability mapping

The in-tree `ups_smt` driver advertises this maximum capability set:

| Capability | Source | Obligation |
|-----------|--------|------------|
| `UPS_CAP_SHUTDOWN` | reg 1540 bit 0 | `commands[].shutdown` with `UPS_CMD_IS_SHUTDOWN` |
| `UPS_CAP_BATTERY_TEST` | reg 1541 bit 0 | `commands[].battery_test` |
| `UPS_CAP_RUNTIME_CAL` | reg 1542 bits 0/1 | `commands[].runtime_cal` + `abort_runtime_cal` |
| `UPS_CAP_CLEAR_FAULTS` | reg 1536 bit 9 | `commands[].clear_faults` |
| `UPS_CAP_MUTE` | reg 1543 bits 2/3 | `commands[].mute` (with `UPS_CMD_IS_MUTE`) + `unmute` |
| `UPS_CAP_BEEP` | reg 1543 bits 0/1 | `commands[].beep_short` + `beep_continuous` |
| `UPS_CAP_HE_MODE` | status bit 13 | UI badge driven from `ups_data.status` |

Capabilities **not** offered by the SMT driver: `UPS_CAP_BYPASS` (no bypass path), `UPS_CAP_FREQ_TOLERANCE` (reg 593 not writable on SMT).

---

## References

- AN-176 (`../vendor/apc/AN176_modbus_implementation.pdf`) — protocol framing, BPI encoding, register-block organisation, USB HID transport notes
- 990-9840B (`../vendor/apc/990-9840_modbus_register_map.pdf`) — full register/bit table for SMX/SMT/SURTD/SRT
- `apc-srt-modbus.md` — companion reference for the SRT line; useful for diffing online-double-conversion behaviour against this line-interactive document
- `../../driver-api.md` — in-tree driver contract this `ups_smt` driver implements
