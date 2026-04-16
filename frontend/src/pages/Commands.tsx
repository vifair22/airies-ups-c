import { useState, useEffect, useCallback } from 'react'
import { useApi, apiPost } from '../hooks/useApi'

/* ── Types ── */

interface UpsStatus {
  connected: boolean
  status?: { raw: number }
}

interface CmdDesc {
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

interface CmdResult {
  result?: string
  error?: string
}

/* ── Toast system ── */

interface Toast { id: number; message: string; type: 'success' | 'error' }
let toastId = 0

function useToast() {
  const [toasts, setToasts] = useState<Toast[]>([])
  const push = useCallback((message: string, type: 'success' | 'error') => {
    const id = ++toastId
    setToasts(prev => [...prev, { id, message, type }])
    setTimeout(() => setToasts(prev => prev.filter(t => t.id !== id)), 4000)
  }, [])
  return { toasts, push }
}

function ToastContainer({ toasts }: { toasts: Toast[] }) {
  if (toasts.length === 0) return null
  return (
    <div className="fixed bottom-4 right-4 z-50 space-y-2">
      {toasts.map(t => (
        <div key={t.id} className={`px-4 py-2.5 rounded-lg border text-sm shadow-lg ${
          t.type === 'error'
            ? 'bg-red-900/90 border-red-700 text-red-200'
            : 'bg-green-900/90 border-green-700 text-green-200'
        }`}>
          {t.message}
        </div>
      ))}
    </div>
  )
}

/* ── Shared UI ── */

function ConfirmModal({ title, body, confirmLabel, confirmVariant = 'default', onConfirm, onCancel, loading }: {
  title: string; body: string; confirmLabel: string
  confirmVariant?: 'default' | 'warn' | 'danger'
  onConfirm: () => void; onCancel: () => void; loading?: boolean
}) {
  const confirmBg = confirmVariant === 'danger'
    ? 'bg-red-900/80 hover:bg-red-800 border-red-700'
    : confirmVariant === 'warn'
      ? 'bg-yellow-900/80 hover:bg-yellow-800 border-yellow-700'
      : 'bg-gray-800 hover:bg-gray-700 border-gray-700'
  const borderColor = confirmVariant === 'danger' ? 'border-red-800' : 'border-gray-700'

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/60" onClick={onCancel}>
      <div className={`bg-gray-900 border ${borderColor} rounded-lg p-6 max-w-md mx-4 shadow-xl`} onClick={e => e.stopPropagation()}>
        <h3 className={`text-lg font-semibold mb-2 ${confirmVariant === 'danger' ? 'text-red-400' : 'text-gray-100'}`}>{title}</h3>
        <p className="text-sm text-gray-400 mb-4">{body}</p>
        <div className="flex justify-end gap-3">
          <button onClick={onCancel}
            className="px-4 py-2 rounded text-sm border border-gray-700 bg-gray-800 hover:bg-gray-700 transition-colors">Cancel</button>
          <button onClick={onConfirm} disabled={loading}
            className={`px-4 py-2 rounded text-sm border transition-colors disabled:opacity-50 ${confirmBg}`}>
            {loading ? '...' : confirmLabel}
          </button>
        </div>
      </div>
    </div>
  )
}

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
    ? 'bg-red-900/80 hover:bg-red-800 border-red-700'
    : cmd.variant === 'warn'
      ? 'bg-yellow-900/60 hover:bg-yellow-800 border-yellow-700'
      : 'bg-gray-800 hover:bg-gray-700 border-gray-700'

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
          isActive ? 'bg-yellow-900/50 text-yellow-400 border border-yellow-700'
          : 'bg-green-900/50 text-green-400 border border-green-700'
        }`}>
          {isActive ? 'ACTIVE' : 'OFF'}
        </span>
        {isActive ? (
          <button onClick={() => setModal('off')}
            className="px-4 py-2 rounded text-sm border bg-gray-800 hover:bg-gray-700 border-gray-700 transition-colors">
            Disable {cmd.display_name}
          </button>
        ) : (
          <button onClick={() => setModal('on')}
            className={`px-4 py-2 rounded text-sm border transition-colors ${
              cmd.variant === 'warn' ? 'bg-yellow-900/60 hover:bg-yellow-800 border-yellow-700'
              : 'bg-gray-800 hover:bg-gray-700 border-gray-700'
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

function ShutdownWorkflow({ onResult }: { onResult: (msg: string, type: 'success' | 'error') => void }) {
  const [modal, setModal] = useState<'dry' | 'real' | null>(null)
  const [loading, setLoading] = useState(false)

  const execute = async (dryRun: boolean) => {
    setLoading(true)
    const res = await apiPost<CmdResult>('/api/cmd', { action: 'shutdown_workflow', dry_run: dryRun })
    onResult(res.result || res.error || 'done', res.error ? 'error' : 'success')
    setModal(null)
    setLoading(false)
  }

  return (
    <>
      <div className="flex gap-2">
        <button onClick={() => setModal('dry')}
          className="px-4 py-2 rounded text-sm border bg-gray-800 hover:bg-gray-700 border-gray-700 transition-colors">
          Dry Run
        </button>
        <button onClick={() => setModal('real')}
          className="px-4 py-2 rounded text-sm border bg-red-900/80 hover:bg-red-800 border-red-700 transition-colors">
          Shutdown
        </button>
      </div>
      {modal === 'dry' && (
        <ConfirmModal title="Run Shutdown Dry Run?"
          body="This will simulate the shutdown workflow without actually shutting anything down. Useful for verifying the shutdown target configuration."
          confirmLabel="Run Dry Run" confirmVariant="default"
          onConfirm={() => execute(true)} onCancel={() => setModal(null)} loading={loading} />
      )}
      {modal === 'real' && (
        <ConfirmModal title="Confirm Shutdown"
          body="This will execute the full shutdown workflow: shut down all configured remote hosts, send shutdown command to UPS, then shut down this system. This action cannot be undone remotely."
          confirmLabel="Shutdown Now" confirmVariant="danger"
          onConfirm={() => execute(false)} onCancel={() => setModal(null)} loading={loading} />
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
        <div className="rounded-lg bg-gray-900 border border-gray-800 text-center py-12">
          <p className="text-gray-500">UPS not connected. Commands unavailable.</p>
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
            <div key={group} className="rounded-lg bg-gray-900 border border-gray-800">
              <div className="px-4 py-2.5 border-b border-gray-800">
                <h3 className="text-xs font-medium text-gray-400 uppercase tracking-wider">{meta.title}</h3>
                {meta.description && <p className="text-xs text-gray-600 mt-0.5">{meta.description}</p>}
              </div>
              <div className="px-4 py-3">
                {cmds.map(cmd => (
                  <div key={cmd.name} className="flex items-center justify-between py-2.5 border-b border-gray-800/60 last:border-0">
                    <div className="mr-4">
                      <span className="text-sm text-gray-200">{cmd.display_name}</span>
                      <p className="text-xs text-gray-500 mt-0.5">{cmd.description}</p>
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
                  <div className="flex items-center justify-between py-2.5 border-b border-gray-800/60 last:border-0">
                    <div className="mr-4">
                      <span className="text-sm text-gray-200">Shutdown Workflow</span>
                      <p className="text-xs text-gray-500 mt-0.5">Executes the full orchestrated shutdown — shuts down all configured hosts, then the UPS itself</p>
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
