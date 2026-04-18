/* Shutdown configuration types */

export interface ShutdownGroup {
  id: number
  name: string
  execution_order: number
  parallel: boolean
  max_timeout_sec: number
  post_group_delay: number
}

export interface ShutdownTarget {
  id: number
  name: string
  method: string
  host: string
  username: string
  credential?: string
  command: string
  timeout_sec: number
  order_in_group: number
  group: string
  group_id: number
  confirm_method: string
  confirm_port: number
  confirm_command: string
  post_confirm_delay: number
}

export interface ShutdownTrigger {
  mode: string
  source: string
  runtime_sec: number
  battery_pct: number
  on_battery: boolean
  delay_sec: number
  field: string
  field_op: string
  field_value: number
}

export interface ShutdownSettings {
  trigger: ShutdownTrigger
  ups_action: { mode: string; register: string; value: number; delay: number }
  controller: { enabled: boolean }
}
