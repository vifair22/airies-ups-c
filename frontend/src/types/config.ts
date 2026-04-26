/* Configuration types */

export type ConfigCategory = 'config' | 'measurement' | 'identity' | 'diagnostic'

export interface ConfigReg {
  name: string
  display_name: string
  unit?: string
  group?: string
  /* Optional for back-compat with older tests; backend always emits this
   * (defaults to 'config' for descriptors that pre-date the category tag). */
  category?: ConfigCategory
  type: 'scalar' | 'bitfield' | 'flags' | 'string'
  raw_value: number
  value: number
  writable: boolean
  /* Set by the backend when the raw value matches a driver-flagged
   * sentinel (e.g. SMT bypass voltage 0xFFFF, load shed config 0x0000).
   * UI should render "N/A" instead of decoding the value. */
  is_sentinel?: boolean
  setting?: string
  setting_label?: string
  date?: string
  min?: number
  max?: number
  step?: number
  max_chars?: number
  strict?: boolean
  options?: { value: number; name: string; label: string }[]
  /* For UPS_CFG_FLAGS: every opts[] entry whose value bits are set in raw. */
  active_flags?: { value: number; name: string; label: string }[]
  /* For UPS_CFG_STRING */
  string_value?: string
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

/* One historical value of a single register, returned by
 * GET /api/config/ups/history?name=<register>. Reverse-chronological.
 *  - 'api'      — written by the operator via the UI
 *  - 'external' — value changed without going through our API (LCD/front panel)
 *  - 'baseline' — first-ever reading, no prior value to diff against
 *  - null       — legacy row written before diff-aware snapshots existed */
export interface ConfigHistoryEntry {
  timestamp: string
  raw_value: number
  display_value: string
  source: 'api' | 'external' | 'baseline' | null
}
