/* UPS status types and constants */

export interface UpsStatus {
  driver: string
  topology?: string
  connected: boolean
  message?: string
  inventory?: {
    model: string
    sku?: string
    serial: string
    firmware: string
    nominal_va: number
    nominal_watts: number
    sog_config: number
  }
  status?: { raw: number; text: string }
  battery?: { charge_pct: number; voltage: number; runtime_sec: number }
  output?: {
    voltage: number; frequency: number | null; current: number
    load_pct: number; energy_wh: number
  }
  outlets?: { mog: number; sog0: number; sog1: number }
  bypass?: { voltage: number; frequency: number | null; status: number }
  input?: { voltage: number; status: number; transfer_high?: number; transfer_low?: number; warn_offset?: number }
  errors?: {
    general: number; power_system: number; battery_system: number
    general_detail: string[]; power_system_detail: string[]; battery_system_detail: string[]
  }
  timers?: { shutdown: number; start: number; reboot: number }
  efficiency?: number
  transfer_reason?: string
  bat_test_status?: number
  rt_cal_status?: number
  bat_lifetime_status?: number
  capabilities?: string[]
  he_mode?: { inhibited: boolean; source: string }
  name?: string
}

/* Status bit constants (mirrors ups.h) */
export const ST = {
  ONLINE:         1 << 1,
  ON_BATTERY:     1 << 2,
  BYPASS:         1 << 3,
  OUTPUT_OFF:     1 << 4,
  FAULT:          1 << 5,
  INPUT_BAD:      1 << 6,
  TEST:           1 << 7,
  PENDING_ON:     1 << 8,
  SHUT_PENDING:   1 << 9,
  COMMANDED:      1 << 10,
  HE_MODE:        1 << 13,
  FAULT_STATE:    1 << 15,
  MAINS_BAD:      1 << 19,
  FAULT_RECOVERY: 1 << 20,
  AVR_BOOST:      1 << 16,
  AVR_TRIM:       1 << 17,
  OVERLOAD:       1 << 21,
} as const

export interface PowerFlowProps {
  statusRaw: number
  inputVoltage: number
  outputVoltage: number
  batteryCharge: number
  batteryVoltage: number
  batteryError: number
  loadPct: number
  efficiency: number
  outputFrequency: number | null
  sensitivity?: 'normal' | 'reduced' | 'low'
  canHE?: boolean
}
