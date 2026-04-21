/* Display formatting helpers for UPS telemetry values */

export const fmtRuntime = (sec: number) => {
  const h = Math.floor(sec / 3600)
  const m = Math.floor((sec % 3600) / 60)
  return h > 0 ? `${h}h ${m}m` : `${m}m`
}

export const fmtWatts = (loadPct: number, nominalW: number) => {
  const w = (loadPct / 100) * nominalW
  return w >= 1000 ? `${(w / 1000).toFixed(2)} kW` : `${Math.round(w)} W`
}

export const fmtVA = (loadPct: number, nominalVA: number) => {
  const va = (loadPct / 100) * nominalVA
  return va >= 1000 ? `${(va / 1000).toFixed(2)} kVA` : `${Math.round(va)} VA`
}

export function humanizeTransfer(reason?: string): string {
  if (!reason) return '--'
  const map: Record<string, string> = {
    SystemInitialization: 'System Init',
    HighInputVoltage: 'High Voltage',
    LowInputVoltage: 'Low Voltage',
    DistortedInput: 'Distorted Input',
    RapidChangeOfInputVoltage: 'Rapid Voltage Change',
    HighInputFrequency: 'High Frequency',
    LowInputFrequency: 'Low Frequency',
    FreqAndOrPhaseDifference: 'Freq/Phase Delta',
    AcceptableInput: 'Input Acceptable',
    AutomaticTest: 'Auto Test',
    TestEnded: 'Test Ended',
    LocalUICommand: 'Front Panel',
    ProtocolCommand: 'Protocol Command',
    LowBatteryVoltage: 'Low Battery',
    GeneralError: 'General Error',
    PowerSystemError: 'Power Error',
    BatterySystemError: 'Battery Error',
    ErrorCleared: 'Error Cleared',
    AutomaticRestart: 'Auto Restart',
    ConfigurationChange: 'Config Change',
  }
  return map[reason] ?? reason
}
