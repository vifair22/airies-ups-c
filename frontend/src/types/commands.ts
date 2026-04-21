/* Command types */

export interface CmdDesc {
  name: string
  display_name: string
  description: string
  group: string
  confirm_title: string
  confirm_body: string
  type: 'simple' | 'toggle'
  variant: 'default' | 'warn' | 'danger'
  is_shutdown?: boolean
  is_mute?: boolean
  status_bit?: number
}

export interface CmdResult {
  result?: string
  error?: string
}

export interface Toast {
  id: number
  message: string
  type: 'success' | 'error'
}
