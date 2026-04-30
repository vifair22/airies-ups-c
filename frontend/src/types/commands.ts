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

export interface ShutdownStep {
  phase:  'phase1' | 'phase2' | 'phase3'
  target: string
  ok:     0 | 1 | 2   /* 0 = ok, 1 = failed, 2 = skipped */
  error:  string
}

export interface CmdResult {
  result?:   string
  error?:    string
  /* Populated only by the shutdown_workflow action. */
  all_ok?:   boolean
  n_steps?:  number
  n_failed?: number
  steps?:    ShutdownStep[]
}

export interface Toast {
  id: number
  message: string
  type: 'success' | 'error'
}
