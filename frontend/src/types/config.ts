/* Configuration types */

export interface ConfigReg {
  name: string
  display_name: string
  unit?: string
  group?: string
  type: string
  raw_value: number
  value: number
  writable: boolean
  setting?: string
  setting_label?: string
  date?: string
  min?: number
  max?: number
  options?: { value: number; name: string; label: string }[]
}

export interface ConfigEntry {
  key: string
  value: string
  type: string
  default_value: string
  description: string
}
