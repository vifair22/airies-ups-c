import { useState, useEffect, useCallback } from 'react'
import { useApi, apiPost, apiGet } from '../hooks/useApi'
import { ConfirmModal, WideModal } from '../components/Modal'
import { useToast, ToastContainer } from '../components/Toast'
import type { UpsStatus } from '../types/ups'
import type { CmdDesc, CmdResult, ShutdownStep, WorkflowStatus } from '../types/commands'

/* ── Simple command button ── */

function SimpleCmd({ cmd, onResult }: { cmd: CmdDesc; onResult: (msg: string, type: 'success' | 'error') => void }) {
  const [open, setOpen] = useState(false)
  const [loading, setLoading] = useState(false)

  const confirm = async () => {
    setLoading(true)
    const res = await apiPost<CmdResult>('/api/cmd', { action: cmd.name })
    onResult(res.result || res.error || 'done', res.error ? 'error' : 'success')
    setOpen(false)
    setLoading(false)
  }

  const bg = cmd.variant === 'danger'
    ? 'bg-red-700 hover:bg-red-600 text-white border-red-800'
    : cmd.variant === 'warn'
      ? 'bg-yellow-600 hover:bg-yellow-700 text-white border-yellow-700'
      : 'bg-field hover:bg-field-hover border-edge-strong'

  return (
    <>
      <button onClick={() => setOpen(true)}
        className={`px-4 py-2 rounded text-sm border transition-colors ${bg}`}>
        {cmd.display_name}
      </button>
      {open && (
        <ConfirmModal title={cmd.confirm_title} body={cmd.confirm_body}
          confirmLabel={cmd.display_name} confirmVariant={cmd.variant}
          onConfirm={confirm} onCancel={() => setOpen(false)} loading={loading} />
      )}
    </>
  )
}

/* ── Toggle command ── */

function ToggleCmd({ cmd, statusRaw, onResult }: {
  cmd: CmdDesc; statusRaw: number
  onResult: (msg: string, type: 'success' | 'error') => void
}) {
  const [modal, setModal] = useState<'on' | 'off' | null>(null)
  const [loading, setLoading] = useState(false)

  const isActive = cmd.status_bit ? !!(statusRaw & cmd.status_bit) : false

  const confirm = async () => {
    if (!modal) return
    setLoading(true)
    const res = await apiPost<CmdResult>('/api/cmd', { action: cmd.name, off: modal === 'off' })
    onResult(res.result || res.error || 'done', res.error ? 'error' : 'success')
    setModal(null)
    setLoading(false)
  }

  return (
    <>
      <div className="flex items-center gap-3">
        <span className={`text-xs font-mono px-2 py-0.5 rounded ${
          isActive ? 'bg-yellow-500/15 text-yellow-700 border border-yellow-600'
          : 'bg-green-500/15 text-green-700 border border-green-600'
        }`}>
          {isActive ? 'ACTIVE' : 'OFF'}
        </span>
        {isActive ? (
          <button onClick={() => setModal('off')}
            className="px-4 py-2 rounded text-sm border bg-field hover:bg-field-hover border-edge-strong transition-colors">
            Disable {cmd.display_name}
          </button>
        ) : (
          <button onClick={() => setModal('on')}
            className={`px-4 py-2 rounded text-sm border transition-colors ${
              cmd.variant === 'warn' ? 'bg-yellow-600 hover:bg-yellow-700 text-white border-yellow-700'
              : 'bg-field hover:bg-field-hover border-edge-strong'
            }`}>
            Enable {cmd.display_name}
          </button>
        )}
      </div>
      {modal && (
        <ConfirmModal
          title={modal === 'on' ? `Enable ${cmd.display_name}?` : `Disable ${cmd.display_name}?`}
          body={cmd.confirm_body}
          confirmLabel={modal === 'on' ? `Enable ${cmd.display_name}` : `Disable ${cmd.display_name}`}
          confirmVariant={modal === 'on' ? cmd.variant : 'default'}
          onConfirm={confirm} onCancel={() => setModal(null)} loading={loading}
        />
      )}
    </>
  )
}

/* ── Shutdown workflow (special case — app-level, not a UPS command) ── */

const phaseLabels: Record<ShutdownStep['phase'], string> = {
  phase1: 'Hosts',
  phase2: 'UPS',
  phase3: 'Controller',
}

function StatusPill({ ok }: { ok: ShutdownStep['ok'] }) {
  const cls =
    ok === 0 ? 'bg-green-500/15 text-green-700 border-green-600'
    : ok === 1 ? 'bg-red-500/15 text-red-700 border-red-600'
    : 'bg-field text-muted border-edge-strong'
  const label = ok === 0 ? 'ok' : ok === 1 ? 'failed' : 'skipped'
  return (
    <span className={`text-xs font-mono px-2 py-0.5 rounded border ${cls}`}>{label}</span>
  )
}

function ShutdownWorkflowModal({ status, dryRun, onClose }: {
  status: WorkflowStatus | null; dryRun: boolean; onClose: () => void
}) {
  const isRunning   = status?.state === 'running' || status === null
  const isCompleted = status?.state === 'completed'
  const steps       = status?.steps   ?? []
  const total       = status?.n_steps  ?? steps.length
  const failed      = status?.n_failed ?? 0
  const allOk       = status?.all_ok ?? false

  const headline = isRunning
    ? (status === null
        ? (dryRun ? 'Starting dry run…' : 'Starting shutdown workflow…')
        : (dryRun ? 'Dry run in progress…' : 'Shutdown workflow in progress…'))
    : isCompleted
      ? (dryRun
          ? (allOk ? 'Dry run complete — no problems detected'
                   : `Dry run found ${failed} problem${failed === 1 ? '' : 's'}`)
          : (allOk ? 'Shutdown initiated'
                   : `Shutdown completed with ${failed} failure${failed === 1 ? '' : 's'}`))
      : 'Workflow idle'

  const headlineCls = isRunning
    ? 'text-primary'
    : (allOk ? 'text-primary' : 'text-red-400')

  const subline = isRunning
    ? (status?.current_target
        ? `Running: ${phaseLabels[status.current_phase as ShutdownStep['phase']] ?? status.current_phase} → ${status.current_target}`
        : 'Workflow starting…')
    : `${total} step${total === 1 ? '' : 's'} executed${dryRun ? ' — no destructive actions taken' : ''}`

  return (
    <WideModal onClose={onClose}>
      <div className="flex items-center gap-3 mb-1">
        <h3 className={`text-lg font-semibold ${headlineCls}`}>{headline}</h3>
        {isRunning && (
          <div className="h-3 w-3 rounded-full bg-primary animate-pulse" aria-label="running" />
        )}
      </div>
      <p className="text-xs text-muted mb-4">{subline}</p>

      {steps.length === 0 ? (
        <p className="text-sm text-muted">
          {isRunning ? 'Waiting for the first step to complete…'
                     : 'No steps were configured or run.'}
        </p>
      ) : (
        <div className="border border-edge rounded overflow-hidden">
          <table className="w-full text-sm">
            <thead className="bg-field text-xs uppercase tracking-wider text-muted">
              <tr>
                <th className="text-left px-3 py-2 font-medium">Phase</th>
                <th className="text-left px-3 py-2 font-medium">Target</th>
                <th className="text-left px-3 py-2 font-medium">Status</th>
                <th className="text-left px-3 py-2 font-medium">Detail</th>
              </tr>
            </thead>
            <tbody>
              {steps.map((s, i) => (
                <tr key={i} className="border-t border-edge/60">
                  <td className="px-3 py-2 text-muted">{phaseLabels[s.phase] ?? s.phase}</td>
                  <td className="px-3 py-2 font-mono text-xs">{s.target}</td>
                  <td className="px-3 py-2"><StatusPill ok={s.ok} /></td>
                  <td className="px-3 py-2 text-xs text-muted">{s.error || '—'}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}

      <div className="flex items-center justify-between mt-4">
        <p className="text-xs text-faint">
          {isRunning && 'Closing this dialog will not stop the workflow — it continues server-side.'}
        </p>
        <button onClick={onClose}
          className="px-4 py-2 rounded text-sm border border-edge-strong bg-field hover:bg-field-hover transition-colors">
          Close
        </button>
      </div>
    </WideModal>
  )
}

function ShutdownWorkflow({ onResult }: { onResult: (msg: string, type: 'success' | 'error') => void }) {
  const [confirm, setConfirm] = useState<'dry' | 'real' | null>(null)
  const [loading, setLoading] = useState(false)
  /* tracking != null → modal is open and we're polling for status. Tracks
   * the workflow_id so we notice if a different run takes over (auto-
   * trigger fires while the user has the dry-run modal open). */
  const [tracking, setTracking] = useState<{ id: number; dryRun: boolean } | null>(null)
  const [status, setStatus]     = useState<WorkflowStatus | null>(null)

  /* Poll /api/shutdown/workflow/status while a workflow is being tracked.
   * Stops on completed (or when the user closes the modal). Uses a
   * recursive setTimeout instead of setInterval so a slow response can't
   * stack up overlapping requests. The effect re-runs when `tracking`
   * changes, so re-pinning to a different workflow_id naturally cancels
   * the old loop via the cleanup. */
  useEffect(() => {
    if (!tracking) return
    const trackedId = tracking.id
    let cancelled = false
    let timer: ReturnType<typeof setTimeout> | null = null

    const tick = async () => {
      if (cancelled) return
      try {
        const s = await apiGet<WorkflowStatus>('/api/shutdown/workflow/status')
        if (cancelled) return
        setStatus(s)

        /* Different run took over (auto-trigger or operator on another
         * tab). Re-pin to the new id; the effect cleanup cancels this
         * loop and a fresh one starts. */
        if (s.workflow_id && s.workflow_id !== trackedId) {
          setTracking({ id: s.workflow_id, dryRun: s.dry_run })
          return
        }

        if (s.state === 'completed') return
      } catch {
        /* Transient fetch failure — fall through to retry. */
      }
      if (!cancelled) timer = setTimeout(tick, 1500)
    }

    tick()
    return () => {
      cancelled = true
      if (timer) clearTimeout(timer)
    }
  }, [tracking])

  const execute = async (dryRun: boolean) => {
    setLoading(true)
    const res = await apiPost<CmdResult>(
      '/api/cmd', { action: 'shutdown_workflow', dry_run: dryRun })
    setConfirm(null)
    setLoading(false)

    if (res.workflow_id) {
      /* 202 (started) or 409 (already running). In either case attach to
       * the workflow and start polling — for 409 the user wanted to run
       * it anyway, so the most useful thing is to show progress on the
       * one that's actually running. */
      if (res.error) onResult(res.error, 'error')
      setStatus(null)
      setTracking({ id: res.workflow_id, dryRun: res.dry_run ?? dryRun })
    } else if (res.error) {
      onResult(res.error, 'error')
    } else {
      onResult(res.result || 'done', 'success')
    }
  }

  const closeModal = () => {
    setTracking(null)
    setStatus(null)
  }

  return (
    <>
      <div className="flex gap-2">
        <button onClick={() => setConfirm('dry')}
          className="px-4 py-2 rounded text-sm border bg-field hover:bg-field-hover border-edge-strong transition-colors">
          Dry Run
        </button>
        <button onClick={() => setConfirm('real')}
          className="px-4 py-2 rounded text-sm border bg-red-700 hover:bg-red-600 text-white border-red-800 transition-colors">
          Shutdown
        </button>
      </div>
      {confirm === 'dry' && (
        <ConfirmModal title="Run Shutdown Dry Run?"
          body="This will validate every step of the shutdown workflow — connectivity to each configured host, the UPS shutdown command, and controller poweroff permission — without executing any destructive action."
          confirmLabel="Run Dry Run" confirmVariant="default"
          onConfirm={() => execute(true)} onCancel={() => setConfirm(null)} loading={loading} />
      )}
      {confirm === 'real' && (
        <ConfirmModal title="Confirm Shutdown"
          body="This will execute the full shutdown workflow: shut down all configured remote hosts, send shutdown command to UPS, then shut down this system. This action cannot be undone remotely."
          confirmLabel="Shutdown Now" confirmVariant="danger"
          onConfirm={() => execute(false)} onCancel={() => setConfirm(null)} loading={loading} />
      )}
      {tracking && (
        <ShutdownWorkflowModal
          status={status}
          /* Trust the daemon's dry_run flag once the first status poll
           * lands — the user might be attaching to a workflow that's
           * a different mode than the button they pressed (409 path). */
          dryRun={status?.dry_run ?? tracking.dryRun}
          onClose={closeModal}
        />
      )}
    </>
  )
}

/* ── Group display names ── */

const groupLabels: Record<string, { title: string; description: string }> = {
  power:       { title: 'Power Control', description: 'UPS power path and shutdown operations' },
  diagnostics: { title: 'Diagnostics', description: 'Battery and hardware verification' },
  alarm:       { title: 'Alarm & Fault Management', description: 'Silence alarms and clear latched fault conditions' },
}

/* ── Main component ── */

export default function Commands() {
  const { data: status } = useApi<UpsStatus>('/api/status', 2000)
  const { data: commands } = useApi<CmdDesc[]>('/api/commands')
  const { toasts, push } = useToast()
  const raw = status?.status?.raw ?? 0

  /* Track continuous beep locally */
  const [continuousActive, setContinuousActive] = useState(false)
  const muteCmd = commands?.find(c => c.is_mute)

  const handleResult = useCallback((name: string) => {
    return (msg: string, type: 'success' | 'error') => {
      push(msg, type)
      if (name === 'beep_continuous' && type === 'success') setContinuousActive(true)
      if (name === 'mute' && type === 'success') setContinuousActive(false)
    }
  }, [push])

  // Also clear continuous when mute detected
  useEffect(() => { void muteCmd }, [muteCmd])

  if (!status?.connected) {
    return (
      <div>
        <h2 className="text-xl font-semibold mb-4">Commands</h2>
        <div className="rounded-lg bg-panel border border-edge text-center py-12">
          <p className="text-muted">UPS not connected. Commands unavailable.</p>

        </div>
      </div>
    )
  }

  /* Group commands by their group field */
  const groups: Record<string, CmdDesc[]> = {}
  if (commands) {
    for (const cmd of commands) {
      if (!groups[cmd.group]) groups[cmd.group] = []
      groups[cmd.group].push(cmd)
    }
  }

  /* Render order: power first (with shutdown workflow injected), then the rest */
  const groupOrder = ['power', ...Object.keys(groups).filter(g => g !== 'power')]

  return (
    <div>
      <h2 className="text-xl font-semibold mb-4">Commands</h2>

      <div className="space-y-4">
        {groupOrder.map(group => {
          const cmds = groups[group]
          if (!cmds) return null
          const meta = groupLabels[group] || { title: group, description: '' }

          return (
            <div key={group} className="rounded-lg bg-panel border border-edge">
              <div className="px-4 py-2.5 border-b border-edge">
                <h3 className="text-xs font-medium text-muted uppercase tracking-wider">{meta.title}</h3>
                {meta.description && <p className="text-xs text-faint mt-0.5">{meta.description}</p>}
              </div>
              <div className="px-4 py-3">
                {cmds.map(cmd => (
                  <div key={cmd.name} className="flex flex-col sm:flex-row sm:items-center sm:justify-between gap-3 py-2.5 border-b border-edge/60 last:border-0">
                    <div className="mr-4">
                      <span className="text-sm text-primary">{cmd.display_name}</span>
                      <p className="text-xs text-muted mt-0.5">{cmd.description}</p>
                    </div>
                    <div className="shrink-0">
                      {cmd.type === 'toggle' ? (
                        <ToggleCmd cmd={cmd} statusRaw={raw} onResult={handleResult(cmd.name)} />
                      ) : cmd.name === 'beep_continuous' && continuousActive ? (
                        <div className="flex gap-2">
                          <SimpleCmd cmd={cmd} onResult={handleResult(cmd.name)} />
                          {muteCmd && <SimpleCmd cmd={{ ...muteCmd, display_name: 'Stop', variant: 'default' }} onResult={handleResult('mute')} />}
                        </div>
                      ) : (
                        <SimpleCmd cmd={cmd} onResult={handleResult(cmd.name)} />
                      )}
                    </div>
                  </div>
                ))}

                {/* Inject shutdown workflow into power group */}
                {group === 'power' && (
                  <div className="flex flex-col sm:flex-row sm:items-center sm:justify-between gap-3 py-2.5 border-b border-edge/60 last:border-0">
                    <div className="mr-4">
                      <span className="text-sm text-primary">Shutdown Workflow</span>
                      <p className="text-xs text-muted mt-0.5">Executes the full orchestrated shutdown — shuts down all configured hosts, then the UPS itself</p>
                    </div>
                    <div className="shrink-0">
                      <ShutdownWorkflow onResult={push} />
                    </div>
                  </div>
                )}
              </div>
            </div>
          )
        })}
      </div>

      <ToastContainer toasts={toasts} />
    </div>
  )
}
