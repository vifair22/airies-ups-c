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
  /* Populated by the shutdown_workflow start path (202 / 409). The
   * full step list and counters now live on /api/shutdown/workflow/status
   * and arrive via WorkflowStatus, not here. */
  workflow_id?: number
  dry_run?:     boolean
}

export type WorkflowState = 'idle' | 'running' | 'completed'

export interface WorkflowStatus {
  state:          WorkflowState
  workflow_id:    number
  dry_run:        boolean
  started_at:     number
  finished_at:    number
  current_phase:  string
  current_target: string
  n_steps:        number
  n_failed:       number
  all_ok:         boolean
  steps:          ShutdownStep[]
}

export interface Toast {
  id: number
  message: string
  type: 'success' | 'error'
}
