# APC Smart-UPS SRT Modbus Reference

Findings from direct testing against an **APC SRT1000XLA** (FW: UPS 16.5, ID=1043) over **serial RTU** (`/dev/ttyUSB0`, 9600 8N1, slave ID 1) using `libmodbus`.

This document covers register addresses, data formats, scaling factors, command registers, and observed behavior that is not well documented in APC's public materials.

---

## Protocol

- **Transport:** Modbus RTU over USB-to-serial adapter
- **Baud:** 9600
- **Parity:** None
- **Data bits:** 8
- **Stop bits:** 1
- **Slave ID:** 1 (configurable via UPS LCD)
- **Function codes used:** FC03 (Read Holding Registers), FC06 (Write Single Register), FC16 (Write Multiple Registers)
- **Byte order:** Big-endian. 32-bit values are stored MSB-first across two consecutive 16-bit registers.

---

## Register Blocks

The UPS organizes registers into discrete blocks. Not all registers within a block are populated — gaps return "Illegal data address." Bulk reads of an entire block succeed when the start address and length match the block boundaries.

| Block | Start | Length | Purpose | Access |
|-------|-------|--------|---------|--------|
| Status | 0 | 27 | UPS status, transfer reason, outlet states, signaling | Read |
| Dynamic | 128 | 44 | Battery, input, output measurements, timers | Read |
| Inventory | 516 | 80 | Model, serial, firmware, ratings | Read (once at startup) |
| Operating Config | 588-595 | varies | Nominal ratings, operating mode, dates | Mixed |
| Settings | 1024-1070 | varies | Transfer voltages, outlet delays, thresholds | Read/Write |
| Commands | 1536-1543 | varies | UPS commands, outlet commands, test commands | Write |

---

## Status Block (Register 0-26)

Read as: `modbus_read_registers(ctx, 0, 27, regs)`

### Registers 0-1: UPS Status Bitfield (uint32)

Constructed as: `(regs[0] << 16) | regs[1]`

| Bit | Mask | Meaning | Notes |
|-----|------|---------|-------|
| 1 | 0x00000002 | Online (OL) | Normal AC operation |
| 2 | 0x00000004 | On Battery (OB) | AC input lost |
| 3 | 0x00000008 | Output on Bypass | SRT only |
| 4 | 0x00000010 | Output Off | Outlets are de-energized |
| 5 | 0x00000020 | General Fault | A fault of any severity |
| 6 | 0x00000040 | Input Not Acceptable | Set together with OB on power loss |
| 7 | 0x00000080 | Self Test in Progress | |
| 8 | 0x00000100 | Pending Output On | Output pending on (set with OutputOff) |
| 9 | 0x00000200 | Shutdown Pending | UPS pending output off |
| 10 | 0x00000400 | Commanded | User-initiated bypass, UPS still functioning. SRT only |
| 13 | 0x00002000 | High Efficiency Mode | UPS is operating in HE/ECO mode |
| 14 | 0x00004000 | Informational Alert | e.g. battery lifetime near end. SRT only |
| 15 | 0x00008000 | Fault State | Operating in a fault state |
| 19 | 0x00080000 | Mains Bad State | State due to bad mains input. SRT only |
| 20 | 0x00100000 | Fault Recovery | Recovering from a fault state. SRT only |
| 21 | 0x00200000 | Overload | SRT only |

**Observed state combinations:**

| Status Value | Meaning |
|-------------|---------|
| `0x00002002` | Online, High Efficiency |
| `0x00000002` | Online, Normal (double conversion) |
| `0x00000244` | On Battery, Input Bad (+ bit 9) |
| `0x00002202` | Online, HE, Shutdown Pending |
| `0x00000110` | Output Off (all outlets dropped) |
| `0x00000408` | Bypass mode |

### Register 2: Input Transfer Reason (uint16 enum)

| Value | Meaning |
|-------|---------|
| 0 | SystemInitialization |
| 1 | HighInputVoltage |
| 2 | LowInputVoltage |
| 3 | DistortedInput |
| 4 | RapidChangeOfInputVoltage |
| 5 | HighInputFrequency |
| 6 | LowInputFrequency |
| 7 | FreqAndOrPhaseDifference |
| 8 | AcceptableInput |
| 9 | AutomaticTest |
| 10 | TestEnded |
| 11 | LocalUICommand |
| 12 | ProtocolCommand |
| 13 | LowBatteryVoltage |
| 14 | GeneralError |
| 15 | PowerSystemError |
| 16 | BatterySystemError |
| 17 | ErrorCleared |
| 18 | AutomaticRestart |
| 19 | DistortedInverterOutput |
| 20 | InverterOutputAcceptable |
| 21 | EPOInterface |
| 22 | InputPhaseDeltaOutOfRange |
| 23 | InputNeutralNotConnected |
| 24 | ATSTransfer |
| 25 | ConfigurationChange |
| 26 | AlertAsserted |
| 27 | AlertCleared |
| 28 | PlugRatingExceeded |
| 29 | OutletGroupStateChange |
| 30 | FailureBypassExpired |

### Registers 3-14: Outlet Group Status Bitfields (uint32 each, 3 regs per group)

Each outlet group occupies 3 registers (2 for the 32-bit bitfield + 1 gap):

| Group | Register Offset | Description |
|-------|----------------|-------------|
| MOG (Main) | 3-4 | Main Outlet Group |
| SOG0 | 6-7 | Switched Outlet Group 0 |
| SOG1 | 9-10 | Switched Outlet Group 1 |
| SOG2 | 12-13 | Switched Outlet Group 2 (not present on SRT1000XLA) |

Constructed as: `(regs[offset] << 16) | regs[offset+1]`

| Bit | Meaning |
|-----|---------|
| 0 | State: ON |
| 1 | State: OFF |
| 2 | Processing Reboot |
| 3 | Processing Shutdown |
| 4 | Processing Sleep |
| 7 | Pending Load Shed |
| 8 | Pending On Delay |
| 9 | Pending Off Delay |
| 10 | Pending On AC Presence |
| 11 | Pending On Min Runtime |
| 14 | Low Runtime |

### Register 18: SimpleSignalingStatus_BF (uint16 bitfield)

| Bit | Meaning | Notes |
|-----|---------|-------|
| 0 | PowerFailure | Set when on battery (output on or off) |
| 1 | ShutdownImminent | UPS committed to disconnecting outputs. **Critical trigger for shutdown workflow** |

### Register 19: GeneralError_BF (uint16 bitfield)

**Note:** This register was previously mislabeled as "Battery System Error" — the actual battery error register is at register 22.

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
| 9 | Oscillator |
| 10 | MeasurementMismatch |
| 11 | Subsystem |

### Registers 20-21: PowerSystemError_BF (uint32 bitfield)

Constructed as: `(regs[20] << 16) | regs[21]`

| Bit | Meaning |
|-----|---------|
| 0 | OutputOverload |
| 1 | OutputShortCircuit |
| 2 | OutputOvervoltage |
| 3 | TransformerDCImbalance |
| 4 | Overtemperature |
| 5 | BackfeedRelay |
| 6 | AVRRelay |
| 7 | PFCInputRelay |
| 8 | OutputRelay |
| 9 | BypassRelay |
| 10 | Fan |
| 11 | PFC |
| 12 | DCBusOvervoltage |
| 13 | Inverter |
| 14 | OverCurrent |

### Register 22: BatterySystemError_BF (uint16 bitfield)

| Bit | Meaning |
|-----|---------|
| 0 | Disconnected |
| 1 | Overvoltage |
| 2 | NeedsReplacement |
| 3 | OvertemperatureCritical |
| 4 | Charger |
| 5 | TemperatureSensor |
| 6 | BusSoftStart |
| 7 | OvertemperatureWarning |
| 8 | GeneralError |
| 9 | Communication |

### Register 23: ReplaceBatteryTestStatus_BF (uint16 bitfield)

| Bit | Meaning |
|-----|---------|
| 0 | Pending |
| 1 | InProgress |
| 2 | Passed |
| 3 | Failed |
| 4 | Refused |
| 5 | Aborted |
| 6 | Source: Protocol |
| 7 | Source: LocalUI |
| 8 | Source: Internal |

### Register 24: RunTimeCalibrationStatus_BF (uint16 bitfield)

| Bit | Meaning |
|-----|---------|
| 0 | Pending |
| 1 | InProgress |
| 2 | Passed |
| 3 | Failed |
| 4 | Refused |
| 5 | Aborted |
| 6 | Source: Protocol |
| 7 | Source: LocalUI |
| 12 | LoadChange |
| 13 | ACInputNotAcceptable |
| 14 | LoadTooLow |

### Register 25: Battery.LifeTimeStatus_BF (uint16 bitfield)

| Bit | Meaning |
|-----|---------|
| 0 | LifeTimeStatusOK |
| 1 | LifeTimeNearEnd |
| 2 | LifeTimeExceeded |
| 3 | LifeTimeNearEndAcknowledged |
| 4 | LifeTimeExceededAcknowledged |

### Register 26: UserInterfaceStatus_BF (uint16 bitfield)

| Bit | Meaning |
|-----|---------|
| 0 | ContinuousTestInProgress |
| 1 | AudibleAlarmInProgress |
| 2 | AudibleAlarmMuted |
| 3 | AnyButtonPressedRecently |

---

## Dynamic Block (Register 128-171)

Read as: `modbus_read_registers(ctx, 128, 44, regs)`

All register offsets below are relative to the base (128). Absolute address = 128 + offset.

### Battery

| Reg | Offset | Length | Variable | Type | Scale | Unit |
|-----|--------|--------|----------|------|-------|------|
| 128-129 | 0-1 | 2 | Battery Runtime | uint32 | raw | Seconds |
| 130 | 2 | 1 | Battery Charge | uint16 | /512 | Percent |
| 131 | 3 | 1 | Battery Positive Voltage | int16 | /32 | Volts DC |
| 132 | 4 | 1 | Battery Negative Voltage | int16 | /32 | Volts DC (SRT only) |
| 133 | 5 | 1 | Battery Date | uint16 | raw | Days since 2000-01-01 |
| 135 | 7 | 1 | Battery Temperature | int16 | /128 | Degrees C |

### Output

| Reg | Offset | Length | Variable | Type | Scale | Unit |
|-----|--------|--------|----------|------|-------|------|
| 136 | 8 | 1 | Output Load | uint16 | /256 | Percent |
| 138 | 10 | 1 | Output Apparent Power | uint16 | /256 | Percent of nominal VA |
| 140 | 12 | 1 | Output Current | uint16 | /32 | Amps |
| 142 | 14 | 1 | Output Voltage | uint16 | /64 | Volts AC |
| 144 | 16 | 1 | Output Frequency | uint16 | /128 | Hz |
| 145-146 | 17-18 | 2 | Output Energy | uint32 | raw | Wh |

### Bypass (SRT only)

| Reg | Offset | Length | Variable | Type | Scale | Unit |
|-----|--------|--------|----------|------|-------|------|
| 147 | 19 | 1 | Bypass Input Status BF | uint16 | bitfield | See Input Status bits below |
| 148 | 20 | 1 | Bypass Voltage | uint16 | /64 | Volts AC |
| 149 | 21 | 1 | Bypass Frequency | uint16 | /128 | Hz |

### Input

| Reg | Offset | Length | Variable | Type | Scale | Unit |
|-----|--------|--------|----------|------|-------|------|
| 150 | 22 | 1 | Input Status BF | uint16 | bitfield | See table below |
| 151 | 23 | 1 | Input Voltage | uint16 | /64 | Volts AC (0xFFFF = N/A) |

**Input.InputStatus_BF / Bypass.InputStatus_BF bits:**

| Bit | Meaning |
|-----|---------|
| 0 | Acceptable |
| 1 | PendingAcceptable |
| 2 | VoltageTooLow |
| 3 | VoltageTooHigh |
| 4 | Distorted |
| 5 | Boost (UPS amplifying input voltage) |
| 6 | Trim (UPS attenuating input voltage) |
| 7 | FrequencyTooLow |
| 8 | FrequencyTooHigh |
| 9 | FreqAndPhaseNotLocked |
| 10 | PhaseDeltaOutOfRange |
| 11 | NeutralNotConnected |
| 15 | PoweringLoad (input is source of power to load) |

### UPS

| Reg | Offset | Length | Variable | Type | Scale | Unit |
|-----|--------|--------|----------|------|-------|------|
| 154 | 26 | 1 | Efficiency | int16 | /128 | Percent (see special values) |

**Efficiency special negative values (raw int16):**

| Raw Value | Meaning |
|-----------|---------|
| -1 (0xFFFF) | Not Available |
| -2 (0xFFFE) | Load Too Low |
| -3 (0xFFFD) | Output Off |
| -4 (0xFFFC) | On Battery |
| -5 (0xFFFB) | In Bypass |
| -6 (0xFFFA) | Battery Charging |
| -7 (0xFFF9) | Poor AC Input |
| -8 (0xFFF8) | Battery Disconnected |

### Timers

| Reg | Offset | Length | Variable | Type | Scale | Notes |
|-----|--------|--------|----------|------|-------|-------|
| 155 | 27 | 1 | Shutdown Timer | int16 | raw | Seconds. -1 = not active |
| 156 | 28 | 1 | Start Timer | int16 | raw | Seconds. -1 = not active |
| 157-158 | 29-30 | 2 | Reboot Timer | int32 | raw | Seconds. -1 = not active |
| 159-170 | 31-42 | 12 | Outlet Group Timers | | | Same pattern per SOG |

---

## Inventory Block (Register 516-595)

Read once at startup: `modbus_read_registers(ctx, 516, 80, regs)`

String registers store 2 ASCII characters per register (MSB = first char, LSB = second char).

| Reg | Offset | Length | Variable | Type | Notes |
|-----|--------|--------|----------|------|-------|
| 512 | - | 1 | UPS ID | uint16 | 1043 on this unit |
| 516-523 | 0-7 | 8 | Firmware Version | string | 16 chars |
| 532-547 | 16-31 | 16 | Model Name | string | 32 chars ("Smart-UPS SRT 1000") |
| 548-563 | 32-47 | 16 | SKU | string | 32 chars (product SKU) |
| 564-571 | 48-55 | 8 | Serial Number | string | 16 chars |
| 572-579 | 56-63 | 8 | Battery SKU | string | 16 chars |
| 580-587 | 64-71 | 8 | External Battery SKU | string | 16 chars (SRT only) |
| 588 | 72 | 1 | Nominal Apparent Power | uint16 | VA (1000 on this unit) |
| 589 | 73 | 1 | Nominal Real Power | uint16 | Watts (900 on this unit) |
| 590 | 74 | 1 | SOGRelayConfigSetting_BF | uint16 BF | Bit 0=MOG, 1=SOG0, 2=SOG1, 3=SOG2, 4=SOG3 |
| 591 | 75 | 1 | Manufacture Date | uint16 | Days since 2000-01-01 |
| 592 | 76 | 1 | Output.VoltageACSetting_BF | uint16 BF | Output voltage setting. Bit 1=VAC120. Read-only. |
| 593 | 77 | 1 | Output.AcceptableFrequencySetting_BF | uint16 BF | See table below. **Read/Write. SRT/SURTD only (not SMT).** |
| 595 | 79 | 1 | Battery Install Date | uint16 | Days since 2000-01-01 (R/W) |
| 596-603 | 80-87 | 8 | UPS Name | string | 16 chars (R/W, user-configurable) |
| 604-611 | 88-95 | 8 | MOG Name | string | 16 chars (R/W) |
| 612-619 | 96-103 | 8 | SOG[0] Name | string | 16 chars (R/W) |
| 620-627 | 104-111 | 8 | SOG[1] Name | string | 16 chars (R/W) |
| 628-635 | 112-119 | 8 | SOG[2] Name | string | 16 chars (R/W) |
| 644-645 | 128-129 | 2 | Output.VoltageACSetting_BF (extended) | uint32 BF | Full 32-bit voltage setting |

---

## Output Frequency Tolerance (Register 593)

**Register 593 is `Output.AcceptableFrequencySetting_BF`** — it controls the acceptable input frequency tolerance for the UPS output. This is NOT an operating mode register, despite the behavioral side effects described below.

The official Schneider Electric register map (990-9840A/B) documents this register as a bitfield where each bit selects a frequency/tolerance combination.

### Accepted Values

| Value | Bit | Doc Name | Meaning | HE Side Effect |
|-------|-----|----------|---------|----------------|
| 1 | 0 | Auto | Automatic 50/60Hz (47-53, 57-63) | Allows HE |
| 2 | 1 | Hz50_0_1 | 50 Hz +/- 0.1 Hz | **Forces 50Hz output — verify load expects 50Hz** |
| 4 | 2 | Hz50_1_0 | 50 Hz +/- 1.0 Hz | Defined in spec, rejected on SRT1000XLA FW 16.5 |
| 8 | 3 | Hz50_3_0 | 50 Hz +/- 3.0 Hz | **Forces 50Hz output — verify load expects 50Hz** |
| 16 | 4 | Hz60_0_1 | 60 Hz +/- 0.1 Hz | Inhibits HE (tolerance too tight for passthrough) |
| 32 | 5 | Hz60_1_0 | 60 Hz +/- 1.0 Hz | Defined in spec, rejected on SRT1000XLA FW 16.5 |
| 64 | 6 | Hz60_3_0 | 60 Hz +/- 3.0 Hz | Allows HE |

### Rejected Values (SRT1000XLA FW 16.5)

Values 4 (Hz50_1_0) and 32 (Hz60_1_0) are defined in the official 990-9840 spec but rejected by this firmware. May work on other models or firmware revisions.

| Value | Result |
|-------|--------|
| 0, 4, 32, 48, 128, 256 | Slave device or server failure |

### Behavior Notes

- **This register controls the output frequency, not just input acceptance.** Writing value 2 (Hz50_0_1) to a 60Hz unit causes the UPS to generate a 50Hz output waveform. This was confirmed with a multimeter showing 50.1Hz on the load side. The UPS handles frequency conversion natively, but **downstream equipment must be rated for the configured frequency** — setting 50Hz on loads that expect 60Hz (or vice versa) can damage equipment.
- **Value 64 (Hz60_3_0) allows HE mode** because the wide tolerance (±3.0Hz) lets the UPS pass input through without tight frequency validation. Transition to HE takes ~30 seconds.
- **Value 16 (Hz60_0_1) inhibits HE mode** because the narrow tolerance (±0.1Hz) is tight enough that real-world utility frequency variation prevents HE engagement. The output remains at 60Hz (confirmed by multimeter). HE drops within ~5 seconds.
- **The UPS slew-rates frequency transitions at ~0.5Hz/sec** rather than stepping, so downstream equipment sees a smooth ramp when the setting changes.
- **The LCD config is authoritative for persistence.** Register 593 acts as a transient override. The LCD front panel will still show whatever was configured locally.
- **No dedicated operating mode register exists** over serial RTU on SRT1000XLA FW 16.5. The NUT apc_modbus driver also does not implement mode control. The only way to reliably change HE/Online mode is through the LCD. Frequency tolerance manipulation is a workaround with the caveats described above.

### Register 592: Output.VoltageACSetting_BF (Read-Only)

Register 592 is the output voltage setting bitfield, NOT an operating mode readback. Observed value of `2` (bit 1 set) corresponds to VAC120 (120V output), which is correct for a US unit. **Use status bit 13 (HE) in register 0-1 to determine actual HE state.**

---

## Settings Block (Register 1024-1070)

These registers are **read/write** and control UPS operational parameters.

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

| Reg | Variable | Unit | Observed Default |
|-----|----------|------|-----------------|
| 1026 | Input Transfer High | Volts | 130 |
| 1027 | Input Transfer Low | Volts | 100 |

### Outlet Delay Settings

Delays control the timing when shutdown/reboot commands use the "Use Off Delay" or "Use On Delay" modifier bits.

**MOG (Main Outlet Group):**

| Reg | Variable | Unit | Default |
|-----|----------|------|---------|
| 1029 | MOG Shutdown Delay | Seconds | 0 |
| 1030 | MOG Start Delay | Seconds | 0 |

**SOG0:**

| Reg | Variable | Unit | Default |
|-----|----------|------|---------|
| 1034 | SOG0 Shutdown Delay | Seconds | 60 |
| 1035 | SOG0 Start Delay | Seconds | 15 |
| 1038 | SOG0 Reboot Delay | Seconds | 420 |

**SOG1:**

| Reg | Variable | Unit | Default |
|-----|----------|------|---------|
| 1039 | SOG1 Shutdown Delay | Seconds | 60 |
| 1040 | SOG1 Start Delay | Seconds | 15 |
| 1043 | SOG1 Reboot Delay | Seconds | 420 |

**SOG2 (not present on SRT1000XLA):**

| Reg | Variable | Unit | Default |
|-----|----------|------|---------|
| 1044 | SOG2 Shutdown Delay | Seconds | 90 |
| 1045 | SOG2 Start Delay | Seconds | 0 |

### Reboot Delay Registers (MOG.StayOffCountdownSetting_4B, uint32)

| Reg | Variable | Default | Notes |
|-----|----------|---------|-------|
| 1031-1032 | MOG Reboot Delay | 8 | R/W |
| 1036-1037 | SOG0 Reboot Delay | 15 | R/W |
| 1041-1042 | SOG1 Reboot Delay | 15 | R/W |
| 1046-1047 | SOG2 Reboot Delay | 8 | R/W |

### Minimum Return Runtime Settings

| Reg | Variable | Unit | Notes |
|-----|----------|------|-------|
| 1033 | MOG MinimumReturnRuntime | Seconds | R/W |
| 1038 | SOG0 MinimumReturnRuntime | Seconds | R/W |
| 1043 | SOG1 MinimumReturnRuntime | Seconds | R/W |
| 1048 | SOG2 MinimumReturnRuntime | Seconds | R/W |

### Load Shed Config Registers

Per-outlet-group bitfields controlling conditions for automatic load shedding. All unconfigured on this unit (no load shedding rules set up via LCD).

**LoadShedConfigSetting_BF bits (per outlet group, 2 regs each):**

| Bit | Meaning |
|-----|---------|
| 0 | UseOffDelay — use TurnOffCountdownSetting when shedding |
| 1 | ManualRestartRequired — use turn off instead of shutdown |
| 3 | TimeOnBattery — shed based on LoadShedTimeOnBatterySetting |
| 4 | RunTimeRemaining — shed based on LoadShedRunTimeRemainingSetting |
| 5 | UPSOverload — turn off immediately on overload (not for MOG) |

| Reg | Variable | Default | Notes |
|-----|----------|---------|-------|
| 1054-1055 | MOG.LoadShedConfigSetting_BF | 0 | R/W |
| 1056-1057 | SOG[0].LoadShedConfigSetting_BF | 0 | R/W |
| 1058-1059 | SOG[1].LoadShedConfigSetting_BF | 0 | R/W |
| 1060-1061 | SOG[2].LoadShedConfigSetting_BF | 0 | R/W |

**Note:** Registers 1054-1061 were intermittently readable during testing — may fail after control plane events or require bulk read.

### Load Shed Threshold Registers

| Reg | Variable | Default | Notes |
|-----|----------|---------|-------|
| 1064 | SOG[0].LoadShedRunTimeRemainingSetting | 32767 | Seconds. 32767 = disabled |
| 1065 | SOG[1].LoadShedRunTimeRemainingSetting | 32767 | Seconds |
| 1066 | SOG[2].LoadShedRunTimeRemainingSetting | 32767 | Seconds |
| 1068 | SOG[0].LoadShedTimeOnBatterySetting | 32767 | Seconds. 32767 = disabled |
| 1069 | SOG[1].LoadShedTimeOnBatterySetting | 32767 | Seconds |
| 1070 | SOG[2].LoadShedTimeOnBatterySetting | 32767 | Seconds |
| 1072 | MOG.LoadShedRunTimeRemainingSetting | 0 | Seconds. R/W |
| 1073 | MOG.LoadShedTimeOnBatterySetting | 32767 | Seconds. R/W |

---

## Command Registers (Write-Only)

### Register 1536-1537: UPS Command Bitfield (uint32, FC16)

Write as: `modbus_write_registers(ctx, 1536, 2, cmd)`

| Bit | Mask | Command | Verified |
|-----|------|---------|----------|
| 3 | 0x00000008 | Restore Factory Settings | No — **do not test** |
| 4 | 0x00000010 | Output Into Bypass | **Yes** — puts UPS into bypass mode |
| 5 | 0x00000020 | Output Out of Bypass | **Yes** — returns from bypass to normal |
| 9 | 0x00000200 | Clear Faults | **Yes** — accepted, no side effects when no faults present |
| 13 | 0x00002000 | Reset Strings | No |
| 14 | 0x00004000 | Reset Logs | No |

Other bit values tested (0x01, 0x02, 0x04, 0x40, 0x80, 0x100) all returned "Slave device or server failure."

### Register 1538-1539: OutletCommand_BF (uint32, FC16)

**NOT FUNCTIONAL OVER SERIAL RTU on SRT1000XLA FW 16.5.**

Every write attempt returns Modbus exception 0x04 ("Slave device or server failure") regardless of command value, target bits, source bits, or function code used. This register is recognized by the UPS (does not return "Illegal data address") but all writes are rejected.

This is believed to be a firmware limitation specific to the serial Modbus interface. NUT's `usbhid-ups` driver successfully controls outlets over USB HID on this same unit.

**Official bit layout (990-9840):**

| Bit | Meaning |
|-----|---------|
| 0 | Cancel (pending actions to targets) |
| 1 | OutputOn |
| 2 | OutputOff |
| 3 | OutputShutdown (off, then on when AC restored) |
| 4 | OutputReboot (off, then on automatically) |
| 5 | ColdBootAllowed (allow turn on without AC) |
| 6 | UseOnDelay |
| 7 | UseOffDelay |
| 8 | Target: MOG |
| 9 | Target: SOG0 |
| 10 | Target: SOG1 |
| 11 | Target: SOG2 |
| 12 | Source: USB |
| 13 | Source: LocalUser |
| 14 | Source: RJ45 |
| 15 | Source: SmartSlot1 |
| 16 | Source: SmartSlot2 |
| 17 | Source: InternalNetwork1 |
| 18 | Source: InternalNetwork2 |

Tested combinations that all failed:
- All command types (reboot, shutdown, on, off, cancel)
- All target combinations (MOG, SOG0, SOG1, all)
- All source bits (USB, Local, RJ45, SmartSlot, Network)
- With and without modifier bits (cold boot, use on/off delay)
- FC06 (single register) and FC16 (multi register)

### Register 1540: SimpleSignalingCommand_BF (uint16, FC06)

**THIS IS THE WORKING SHUTDOWN COMMAND OVER SERIAL RTU.**

Write as: `modbus_write_register(ctx, 1540, value)`

| Bit | Command | Verified |
|-----|---------|----------|
| 0 | RequestShutdown | **Yes** — see behavior notes below |
| 1 | RemoteOff (equivalent to holding power off button) | No |
| 2 | RemoteOn (equivalent to pressing ON power button) | No |

**Observed shutdown behavior:**

1. Write `0x0001` to register 1540
2. Status changes to `0x00002202` (OL + HE + Shutdown Pending, bit 9 set)
3. All outlet groups remain ON during countdown
4. After ~60 seconds, outlets drop: status → `0x00000110` (Output Off)
5. MOG, SOG0, SOG1 all report OFF
6. UPS internal controller remains powered (Modbus communication continues)
7. If AC mains present: UPS waits for minimum runtime/charge threshold
8. MOG comes back ON first
9. SOG0 and SOG1 follow ~15 seconds later
10. Status returns to `0x00000002` (OL) then eventually `0x00002002` (OL + HE) after stabilization

**Total observed cycle time:** ~60s countdown + ~15s outlets off + ~15s staggered restore = ~90s

**The ~60s countdown is fixed and NOT configurable.** Tested by writing a 10s value to the MOG shutdown delay register (1029) before issuing the shutdown command — the countdown was still ~63 seconds. The outlet delay registers (1029-1045) only apply to the OutletCommand register (1538), which does not work over serial RTU. The SimpleSignalingCommand uses its own hardcoded timing.

**Behavior without AC mains (verified):**

Tested by pulling AC mains, then issuing shutdown command while on battery:

1. Command accepted while on battery — write succeeds
2. Sig register 18 immediately sets bit 1 (LB/ShutdownImminent, `0x0003`) in response to the shutdown command, not battery depletion
3. ~60s countdown, outlets drop
4. **Outlets stay OFF** — UPS internal controller remains alive on battery, Modbus communication continues
5. Outlets remain off as long as AC is absent — UPS does not attempt to restore
6. When AC returns: MOG comes back ON first, SOGs follow ~16s later
7. If AC never returns, battery eventually depletes and UPS goes fully dead; on AC restore it cold boots from zero

Observed timeline:
- 18:56:47 — AC pulled, OB detected
- 18:56:57 — Shutdown command sent (after 10s stabilize)
- 18:58:00 — Outlets OFF (~60s after command)
- 18:58:00 to 18:59:34 — Outlets remained OFF, no AC, UPS alive on battery
- 18:59:34 — AC restored, MOG ON
- 18:59:50 — SOG0 + SOG1 ON

This effectively replicates the NUT `shutdown.reboot` command behavior and confirms the command is safe to use as the primary shutdown mechanism in the real-world power-failure scenario.

### Register 1541: Battery Test Command (uint16, FC06)

| Bit | Command | Verified |
|-----|---------|----------|
| 0 | Start Battery Test | **Yes** |
| 1 | Abort Battery Test | No (test completed before abort needed) |

Register is readable (returns 0x0000 when idle). **Verified behavior:**

1. Write `0x0001` → test starts immediately
2. Status changes to `0x00000084` (OB + TEST, bits 2 and 7)
3. Register 23 (ReplaceBatteryTestStatus) shows `0x0042` = In Progress (bit 1) + Source: Protocol (bit 6)
4. UPS runs on battery for ~20 seconds, battery voltage drops
5. Test completes, status returns to OL
6. Register 23 shows `0x0044` = Passed (bit 2) + Source: Protocol (bit 6)
7. If test fails, register 23 bit 3 (Failed) would be set instead

**Note:** Battery test knocks UPS out of HE mode. HE re-engages after stabilization (~5 min).

### Register 1542: Runtime Calibration Command (uint16, FC06)

| Bit | Command | Verified |
|-----|---------|----------|
| 0 | Start Runtime Calibration | No |
| 1 | Abort Runtime Calibration | No |

Register is readable. Write not tested (runtime calibration deeply discharges the battery).

### Register 1543: User Interface Command (uint16, FC06)

| Bit | Command | Verified |
|-----|---------|----------|
| 0 | Short LED/Beeper Test | **Yes** — beep + orange LED |
| 1 | Continuous Test | **Yes** — single tone + red LED |
| 2 | Mute All Active Audible Alarms | **Yes** — accepted |
| 3 | Cancel Mute | **Yes** — accepted |
| 5 | Acknowledge Battery Alarms | No |
| 6 | Acknowledge Site Wiring Alarm | No |

Register is readable. The mute/cancel mute commands are useful for silencing the UPS beeper during a managed shutdown where the app is already handling the situation.

---

## Observed State Machine Transitions

### Power Failure (AC Lost)

```
Online (0x00002002 OL HE)
  → On Battery (0x00000244 OB INPUT_BAD)
    Transfer reason: 4 (RapidChangeOfInputVoltage)
    Sig reg 18: 0x0001 (bit 0 active)
    Input voltage: 0.0V
    Battery voltage begins dropping
    Runtime countdown begins
```

### Power Restore (AC Returns)

```
On Battery (0x00000244)
  → Input voltage reappears (e.g., 122.3V) while still showing OB
  → Online (0x00000002 OL)
    Transfer reason: 8 (AcceptableInput)
    Sig reg 18: 0x0000 (cleared)
    Battery voltage rebounds
    Runtime recalculates upward
    HE bit may not immediately re-engage (stabilization period)
```

### Shutdown Command While Online (Reg 1540 = 0x0001)

```
Online (0x00002002 OL HE)
  → Shutdown Pending (0x00002202 OL HE + bit 9)
    ~60 second countdown
    Outlets remain energized
  → Output Off (0x00000110 OFF)
    All outlets de-energized
    MOG, SOG0, SOG1 report OFF
    UPS controller still alive on Modbus
  → [AC present] Restore sequence:
    → MOG ON first (0x00000002 OL)
    → SOG0, SOG1 ON ~15s later
    → HE re-engages after ~5 min stabilization
```

### Shutdown Command While On Battery (Reg 1540 = 0x0001)

```
On Battery (0x00000244 OB INBAD)
  → Shutdown command accepted
  → Sig reg 18 immediately sets bit 1 (LB/ShutdownImminent): 0x0001 → 0x0003
    NOTE: The LB flag is triggered by the shutdown command itself, not by battery depletion
  → Shutdown Pending (0x00000244 OB INBAD + bit 9)
    ~60 second countdown
    Outlets remain energized
  → Output Off (0x00000150 OFF INBAD)
    All outlets de-energized
    UPS controller alive on battery, Modbus continues
  → [AC absent] Outlets remain OFF indefinitely
    UPS does not attempt to restore while AC is absent
    Battery slowly drains powering internal controller only
  → [AC restored] Same restore sequence:
    → MOG ON first (0x00000002 OL)
    → SOG0, SOG1 ON ~16s later
```

### Power Failure — Online Mode (no HE)

```
Online (0x00000002 OL)
  → On Battery (0x00000244 OB INPUT_BAD)
    Transfer reason: 3 (DistortedInput) — may vary
    Sig reg 18: 0x0001 (bit 0 active)
    Identical status bits to HE mode power failure
```

**Confirmed:** OB behavior is identical regardless of whether HE or Online mode is active. Same status bits, same Sig register behavior, same shutdown command flow. The app does not need to handle these modes differently.

### Battery Test (Reg 1541 = 0x0001)

```
Online (0x00002002 OL HE) or (0x00000002 OL)
  → Write 0x0001 to reg 1541
  → Self Test (0x00000084 OB + TEST)
    Reg 23 = 0x0042 (In Progress + Source: Protocol)
    UPS runs on battery for ~20 seconds
    Battery voltage drops during test
  → Test Complete — returns to OL
    Reg 23 = 0x0044 (Passed + Source: Protocol)
    HE mode knocked out, re-engages after ~5 min
```

### Bypass Toggle (Reg 1536)

```
Online (0x00002002 OL HE)
  → Write 0x00000010 to reg 1536 (Output Into Bypass)
  → Bypass (0x00000408 BYP)
    HE flag clears
  → Write 0x00000020 to reg 1536 (Output Out of Bypass)
  → Online (0x00000002 OL)
    HE re-engages after stabilization period
```

---

## Hardware Notes

- **Model:** APC SRT1000XLA (Smart-UPS SRT 1000VA / 900W)
- **Firmware:** UPS 16.5, ID=1043
- **Serial:** AS2434290511
- **USB ID:** 051d:0003 (American Power Conversion UPS)
- **Outlet groups present:** MOG + SOG0 + SOG1 (reg 590 = 0x0006)
- **Manufacture date:** ~Aug 2024 (reg 591 = 8998 days since 2000-01-01)
- **Connection:** USB-to-serial adapter appears as `/dev/ttyUSB0` on Raspberry Pi
- **Modbus must be enabled** via UPS front panel LCD (Configuration menu). Only enable/disable and address are available — no read-only vs read-write distinction.

---

## Known Limitations

1. **Outlet Command register (1538) does not accept writes over serial RTU.** Use register 1540 (SimpleSignalingCommand) for shutdown instead. This means per-outlet-group control is not available over serial — the shutdown command affects all outlets.

2. **Frequency tolerance changes (reg 593) affect the output waveform.** The UPS will convert to whatever frequency is configured, but **the load must be rated for that frequency**. Setting 50Hz on equipment expecting 60Hz (or vice versa) can damage downstream devices. The UPS slew-rates frequency transitions at ~0.5Hz/sec. Status bit 13 (HE) may take ~5 minutes to stabilize after a tolerance change.

3. **Rapid-fire config register writes will reset the UPS control plane.** Writing to multiple config registers (1024-1073) without delays causes a watchdog timeout or buffer overflow in the UPS firmware, resulting in a control plane reboot (outlets stay up, but the internal controller bounces and Modbus communication is briefly lost). The NUT `apc_modbus` driver works around this with a mandatory **100ms `usleep()` after every register write** (see NUT source line ~1687: *"There seem to be some communication problems if we don't wait after writing"*). For safety, use at least 100-200ms delay between consecutive writes. This is not an issue for normal monitoring operations — only relevant when writing config/settings.

4. **The `usbhid-ups` NUT driver has a known 100% CPU bug** with this UPS. If using USB HID for any purpose, be aware of this issue.

5. **After a bypass cycle or power event, HE mode may not re-engage automatically** even if configured. The status bit clears and takes several minutes to return once the system stabilizes.

---

## Protocol Verification Block (Register 2048-2061)

Test registers for verifying Modbus communication. Defined in 990-9840 but not scanned during original testing (address range was 0-8192+, these may have been missed or returned 0xFFFF).

| Reg | Length | Variable | Type | Expected Value |
|-----|--------|----------|------|----------------|
| 2048-2049 | 2 | ModbusMapID | ASCII | Map identifier string |
| 2050-2053 | 4 | TestString | ASCII | "ABCD EFGH" |
| 2054-2055 | 2 | Test4BNumber1 | uint32 | 305,419,896 (0x12345678) |
| 2056-2057 | 2 | Test4BNumber2 | int32 | -305,419,896 |
| 2058 | 1 | Test2BNumber1 | uint16 | 12,345 |
| 2059 | 1 | Test2BNumber2 | int16 | -12,345 |
| 2060 | 1 | TestBPINumber1 | int16 (/64) | 192.96 (raw: 12,350) |
| 2061 | 1 | TestBPINumber2 | int16 (/64) | -192.96 (raw: -12,350) |

---

## Complete Register Scan Summary

Full scan of registers 0-8192+ performed on SRT1000XLA FW 16.5. Populated (non-0xFFFF) register regions:

| Range | Count | Purpose |
|-------|-------|---------|
| 0-26 | 27 | Status block |
| 128-170 | ~30 | Dynamic measurements & timers |
| 512-513 | 2 | UPS ID |
| 516-635 | ~80 | Inventory (firmware, model, serial, SKU, ratings, names) |
| 588-595 | 8 | Inventory (ratings, dates, operating mode) |
| 1024-1073 | ~48 | Config (test interval, transfer voltages, delays, load shed) |
| 1540-1543 | 4 | Command registers (write-only except for read of current state) |
| 2048-2061 | 14 | Protocol verification (test values) |

Everything outside these ranges returned either "Illegal data address" (register does not exist) or `0xFFFF` (reserved/unused padding).

---

## References

- APC Application Note #176 (Modbus for Smart-UPS) — internal doc MPAO-98KJ7F_EN
- APC Register Map 990-9840A (SPD_LFLG-A32G3L_EN)
- NUT apc_modbus driver source: https://github.com/networkupstools/nut/blob/master/drivers/apc_modbus.c
- NUT apc_modbus header: https://github.com/networkupstools/nut/blob/master/drivers/apc_modbus.h
- Schneider Electric Community: SRT 2200 Modbus outlet command discussion
