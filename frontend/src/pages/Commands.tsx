import { useState, useEffect, useCallback } from 'react'
import { useApi, apiPost } from '../hooks/useApi'

/* ── Types ── */

interface UpsStatus {
  driver: string
  connected: boolean
  status?: { raw: number }
  capabilities?: string[]
  bypass?: {
    voltage: number; frequency: number; status: number
    voltage_high: number; voltage_low: number
  }
  errors?: { general: number; power_system: number; battery_system: number }
}

interface CmdResult {
  result?: string
  error?: string
}

const ST = {
  BYPASS:     1 << 3,
  FAULT:      1 << 5,
  TEST:       1 << 7,
  COMMANDED:  1 << 10,
} as const

const BATERR_DISCONNECTED = 1 << 0

/* ── Command execution ── */

async function execCmd(action: string, extra?: Record<string, unknown>): Promise<CmdResult> {
  try {
    return await apiPost<CmdResult>('/api/cmd', { action, ...extra })
  } catch {
    return { error: 'request failed' }
  }
}

/* ── Toast system ── */

interface Toast {
  id: number
  message: string
  type: 'success' | 'error'
}

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
        <div key={t.id} className={`px-4 py-2.5 rounded-lg border text-sm shadow-lg animate-fade-in ${
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

function Section({ title, description, children }: {
  title: string; description?: string; children: React.ReactNode
}) {
  return (
    <div className="rounded-lg bg-gray-900 border border-gray-800">
      <div className="px-4 py-2.5 border-b border-gray-800">
        <h3 className="text-xs font-medium text-gray-400 uppercase tracking-wider">{title}</h3>
        {description && <p className="text-xs text-gray-600 mt-0.5">{description}</p>}
      </div>
      <div className="px-4 py-3">{children}</div>
    </div>
  )
}

function CommandRow({ label, description, children }: {
  label: string; description?: string; children: React.ReactNode
}) {
  return (
    <div className="flex items-center justify-between py-2.5 border-b border-gray-800/60 last:border-0">
      <div className="mr-4">
        <span className="text-sm text-gray-200">{label}</span>
        {description && <p className="text-xs text-gray-500 mt-0.5">{description}</p>}
      </div>
      <div className="shrink-0">{children}</div>
    </div>
  )
}

function ConfirmModal({ title, body, confirmLabel, confirmVariant = 'warn', onConfirm, onCancel, loading }: {
  title: string
  body: React.ReactNode
  confirmLabel: string
  confirmVariant?: 'warn' | 'danger' | 'default'
  onConfirm: () => void
  onCancel: () => void
  loading?: boolean
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
        <h3 className={`text-lg font-semibold mb-2 ${confirmVariant === 'danger' ? 'text-red-400' : 'text-gray-100'}`}>
          {title}
        </h3>
        <div className="text-sm text-gray-400 mb-4">{body}</div>
        <div className="flex justify-end gap-3">
          <button onClick={onCancel}
            className="px-4 py-2 rounded text-sm border border-gray-700 bg-gray-800 hover:bg-gray-700 transition-colors">
            Cancel
          </button>
          <button onClick={onConfirm} disabled={loading}
            className={`px-4 py-2 rounded text-sm border transition-colors disabled:opacity-50 ${confirmBg}`}>
            {loading ? '...' : confirmLabel}
          </button>
        </div>
      </div>
    </div>
  )
}

/* ── Command button with built-in modal ── */

function ModalCmdButton({ label, modalTitle, modalBody, confirmLabel, action, extra, variant = 'default', confirmVariant, disabled, onResult }: {
  label: string
  modalTitle: string
  modalBody: React.ReactNode
  confirmLabel: string
  action: string
  extra?: Record<string, unknown>
  variant?: 'default' | 'danger' | 'warn'
  confirmVariant?: 'warn' | 'danger' | 'default'
  disabled?: boolean
  onResult: (msg: string, type: 'success' | 'error') => void
}) {
  const [open, setOpen] = useState(false)
  const [loading, setLoading] = useState(false)

  const confirm = async () => {
    setLoading(true)
    const res = await execCmd(action, extra)
    const msg = res.result || res.error || 'done'
    onResult(msg, res.error ? 'error' : 'success')
    setOpen(false)
    setLoading(false)
  }

  const bg = variant === 'danger'
    ? 'bg-red-900/80 hover:bg-red-800 border-red-700'
    : variant === 'warn'
      ? 'bg-yellow-900/60 hover:bg-yellow-800 border-yellow-700'
      : 'bg-gray-800 hover:bg-gray-700 border-gray-700'

  return (
    <>
      <button onClick={() => setOpen(true)} disabled={disabled}
        className={`px-4 py-2 rounded text-sm border transition-colors disabled:opacity-40 disabled:cursor-not-allowed ${bg}`}>
        {label}
      </button>
      {open && (
        <ConfirmModal
          title={modalTitle}
          body={modalBody}
          confirmLabel={confirmLabel}
          confirmVariant={confirmVariant ?? (variant === 'default' ? 'default' : variant)}
          onConfirm={confirm}
          onCancel={() => setOpen(false)}
          loading={loading}
        />
      )}
    </>
  )
}

/* ── Bypass toggle with state awareness ── */

function BypassControl({ raw, caps, bypass, batteryDisconnected, onResult }: {
  raw: number; caps: string[]
  bypass?: UpsStatus['bypass']
  batteryDisconnected: boolean
  onResult: (msg: string, type: 'success' | 'error') => void
}) {
  const [modal, setModal] = useState<'enable' | 'disable' | null>(null)
  const [loading, setLoading] = useState(false)

  if (!caps.includes('bypass')) return null

  const isInBypass = !!(raw & ST.BYPASS)
  const isFault = !!(raw & ST.FAULT)
  const vHigh = bypass?.voltage_high ?? 140
  const vLow = bypass?.voltage_low ?? 90

  const confirm = async () => {
    if (!modal) return
    setLoading(true)
    const res = await execCmd(modal === 'enable' ? 'bypass_on' : 'bypass_off')
    const msg = res.result || res.error || 'done'
    onResult(msg, res.error ? 'error' : 'success')
    setModal(null)
    setLoading(false)
  }

  return (
    <>
      <CommandRow
        label="Bypass Mode"
        description="Routes utility power directly to the output, bypassing the UPS power conditioning. Used for UPS maintenance or to reduce heat and power consumption."
      >
        <div className="flex items-center gap-3">
          <span className={`text-xs font-mono px-2 py-0.5 rounded ${
            isInBypass ? 'bg-yellow-900/50 text-yellow-400 border border-yellow-700'
            : 'bg-green-900/50 text-green-400 border border-green-700'
          }`}>
            {isInBypass ? 'ACTIVE' : 'OFF'}
          </span>

          {isInBypass ? (
            <button onClick={() => setModal('disable')}
              className="px-4 py-2 rounded text-sm border bg-gray-800 hover:bg-gray-700 border-gray-700 transition-colors">
              Disable Bypass
            </button>
          ) : (
            <button onClick={() => setModal('enable')} disabled={isFault}
              className="px-4 py-2 rounded text-sm border bg-yellow-900/60 hover:bg-yellow-800 border-yellow-700 transition-colors disabled:opacity-40 disabled:cursor-not-allowed">
              Enable Bypass
            </button>
          )}
        </div>
      </CommandRow>

      {modal && (
        <ConfirmModal
          title={modal === 'enable' ? 'Enable Bypass?' : 'Disable Bypass?'}
          body={modal === 'enable' ? (<>
            <p className="mb-2">Enabling bypass routes utility power directly to the output without conditioning.</p>
            <p className="mb-2">The UPS will pass through utility voltage between <span className="text-gray-200 font-mono">{vLow}V</span> and <span className="text-gray-200 font-mono">{vHigh}V</span>. Outside this window, the UPS will attempt to catch the load on the inverter.</p>
            <p className="text-red-400 font-medium">Battery must be present for the UPS to catch the load during a power event in bypass mode.</p>
            {batteryDisconnected && (
              <p className="mt-2 text-red-400 font-bold">Battery is currently disconnected — the load WILL drop if utility voltage leaves the bypass window.</p>
            )}
          </>) : (
            <p>Disabling bypass returns the UPS to normal operation. Power will be conditioned through the UPS before reaching the output.</p>
          )}
          confirmLabel={modal === 'enable' ? 'Enable Bypass' : 'Disable Bypass'}
          confirmVariant={modal === 'enable' ? 'warn' : 'default'}
          onConfirm={confirm}
          onCancel={() => setModal(null)}
          loading={loading}
        />
      )}
    </>
  )
}

/* ── Main component ── */

export default function Commands() {
  const { data: status } = useApi<UpsStatus>('/api/status', 2000)
  const { toasts, push } = useToast()
  const caps = status?.capabilities || []
  const has = (c: string) => caps.includes(c)
  const raw = status?.status?.raw ?? 0
  const batteryDisconnected = !!((status?.errors?.battery_system ?? 0) & BATERR_DISCONNECTED)

  /* Track continuous beep state locally */
  const [continuousBeepActive, setContinuousBeepActive] = useState(false)

  /* Clear continuous beep flag when UI status shows it stopped (mute was applied) */
  const uiTestBit = 0 // We don't have ui_status exposed yet, so track locally
  useEffect(() => { void uiTestBit }, [])

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

  const isTestRunning = !!(raw & ST.TEST)

  return (
    <div>
      <h2 className="text-xl font-semibold mb-4">Commands</h2>

      <div className="space-y-4">
        {/* Power Control */}
        <Section title="Power Control" description="UPS power path and shutdown operations">
          <BypassControl raw={raw} caps={caps} bypass={status.bypass} batteryDisconnected={batteryDisconnected} onResult={push} />

          <CommandRow label="Shutdown Workflow" description="Executes the full orchestrated shutdown — shuts down all configured hosts, then the UPS itself">
            <div className="flex gap-2">
              <ModalCmdButton onResult={push}
                label="Dry Run" action="shutdown" extra={{ dry_run: true }}
                modalTitle="Run Shutdown Dry Run?"
                modalBody="This will simulate the shutdown workflow without actually shutting anything down. Useful for verifying the shutdown target configuration."
                confirmLabel="Run Dry Run"
                confirmVariant="default"
              />
              <ModalCmdButton onResult={push}
                label="Shutdown" action="shutdown" variant="danger"
                modalTitle="Confirm Shutdown"
                modalBody={<>
                  <p className="mb-2">This will execute the full shutdown workflow:</p>
                  <ol className="list-decimal list-inside space-y-1 mb-3">
                    <li>Shut down all configured remote hosts</li>
                    <li>Send shutdown command to UPS</li>
                    <li>Shut down this system</li>
                  </ol>
                  <p className="text-red-400/80">This action cannot be undone remotely.</p>
                </>}
                confirmLabel="Shutdown Now"
                confirmVariant="danger"
              />
            </div>
          </CommandRow>
        </Section>

        {/* Diagnostics */}
        <Section title="Diagnostics" description="Battery and hardware verification">
          {has('battery_test') && (
            <CommandRow label="Battery Test" description="Run a quick self-test to verify battery health">
              <ModalCmdButton onResult={push}
                label={isTestRunning ? 'Test Running...' : 'Start Test'}
                action="battery_test" disabled={isTestRunning}
                modalTitle="Start Battery Test?"
                modalBody="This will run a brief self-test where the UPS switches to battery power momentarily to verify battery health."
                confirmLabel="Start Test"
                confirmVariant="default"
              />
            </CommandRow>
          )}
          {has('runtime_cal') && (
            <CommandRow label="Runtime Calibration" description="Deep discharge to recalibrate the runtime estimate">
              <ModalCmdButton onResult={push}
                label="Start Calibration" action="runtime_cal" variant="warn"
                modalTitle="Start Runtime Calibration?"
                modalBody={<>
                  <p className="mb-2">Runtime calibration deeply discharges the battery to recalibrate the runtime estimate. The UPS will be on battery power for the entire duration.</p>
                  <p className="mb-2 text-yellow-400/80">The battery will take a significant amount of time to recharge after calibration completes.</p>
                  <p className="text-red-400/80">If the battery is degraded, the UPS may not be able to sustain the load for the full calibration — this could result in an unclean shutdown.</p>
                </>}
                confirmLabel="Start Calibration"
                confirmVariant="warn"
              />
            </CommandRow>
          )}
          {has('beep') && (
            <CommandRow label="Beep Test" description="Verify the audible alarm is functional">
              <div className="flex gap-2">
                <ModalCmdButton onResult={push}
                  label="Short" action="beep_short"
                  modalTitle="Short Beep Test?"
                  modalBody="This will emit a brief beep to verify the audible alarm."
                  confirmLabel="Beep"
                  confirmVariant="default"
                />
                {continuousBeepActive ? (
                  <ModalCmdButton onResult={(msg, type) => { push(msg, type); setContinuousBeepActive(false) }}
                    label="Stop Continuous" action="mute" variant="warn"
                    modalTitle="Stop Continuous Test?"
                    modalBody="This will mute the alarm and stop the continuous beep/LED test."
                    confirmLabel="Stop"
                    confirmVariant="default"
                  />
                ) : (
                  <ModalCmdButton onResult={(msg, type) => { push(msg, type); if (type === 'success') setContinuousBeepActive(true) }}
                    label="Continuous" action="beep_continuous" variant="warn"
                    modalTitle="Continuous Beep Test?"
                    modalBody="This starts a continuous beep and LED test. The alarm will sound until you stop it."
                    confirmLabel="Start Continuous Test"
                    confirmVariant="warn"
                  />
                )}
              </div>
            </CommandRow>
          )}
        </Section>

        {/* Alarm & Fault Management */}
        <Section title="Alarm & Fault Management" description="Silence alarms and clear latched fault conditions">
          {has('mute') && (
            <CommandRow label="Audible Alarm" description="Mute or unmute the UPS alarm buzzer">
              <div className="flex gap-2">
                <ModalCmdButton onResult={(msg, type) => { push(msg, type); setContinuousBeepActive(false) }}
                  label="Mute" action="mute"
                  modalTitle="Mute Alarm?"
                  modalBody="This will silence the UPS audible alarm. The alarm will remain muted until a new alarm condition occurs or you unmute it."
                  confirmLabel="Mute"
                  confirmVariant="default"
                />
                <ModalCmdButton onResult={push}
                  label="Unmute" action="unmute"
                  modalTitle="Unmute Alarm?"
                  modalBody="This will re-enable the UPS audible alarm. Any active alarm conditions will immediately sound."
                  confirmLabel="Unmute"
                  confirmVariant="default"
                />
              </div>
            </CommandRow>
          )}
          {has('clear_faults') && (
            <CommandRow label="Clear Faults" description="Reset latched fault flags">
              <ModalCmdButton onResult={push}
                label="Clear" action="clear_faults"
                modalTitle="Clear Fault Flags?"
                modalBody="This will reset all latched fault indicators on the UPS."
                confirmLabel="Clear Faults"
                confirmVariant="default"
              />
            </CommandRow>
          )}
        </Section>
      </div>

      <ToastContainer toasts={toasts} />
    </div>
  )
}
