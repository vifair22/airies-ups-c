/* Telemetry types */

export interface TelemetryPoint {
  timestamp: string
  charge_pct: number
  load_pct: number
  input_voltage: number
  output_voltage: number
  battery_voltage: number
  output_frequency: number
  output_current: number
  runtime_sec: number
  efficiency: number
}

export interface MetricDef {
  key: keyof TelemetryPoint
  label: string
  unit: string
  color: string
  format?: (v: number) => string
}
