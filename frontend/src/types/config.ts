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
  step?: number
  max_chars?: number
  strict?: boolean
  options?: { value: number; name: string; label: string }[]
}

/* Structured 400 response from POST /api/config/ups when validation
 * rejects the value before it reaches the UPS — see
 * src/api/routes/config.c:handle_config_ups_set. */
export interface ConfigWriteError {
  error: 'out_of_range' | string
  register: string
  attempted_value: number
  min?: number
  max?: number
  step?: number
  accepted_values?: number[]
}

export interface ConfigEntry {
  key: string
  value: string
  type: string
  default_value: string
  description: string
}
