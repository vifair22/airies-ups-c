import { useState } from 'react'
import { useApi, apiPost } from '../hooks/useApi'

interface UpsStatus {
  capabilities?: string[]
  connected: boolean
}

interface CmdResult {
  result?: string
  error?: string
}

function CmdButton({ label, action, extra, confirm: needsConfirm, variant = 'default' }: {
  label: string
  action: string
  extra?: Record<string, unknown>
  confirm?: string
  variant?: 'default' | 'danger' | 'warn'
}) {
  const [result, setResult] = useState<string | null>(null)
  const [loading, setLoading] = useState(false)

  const run = async () => {
    if (needsConfirm && !window.confirm(needsConfirm)) return
    setLoading(true)
    try {
      const res = await apiPost<CmdResult>('/api/cmd', { action, ...extra })
      setResult(res.result || res.error || 'done')
    } catch {
      setResult('failed')
    }
    setLoading(false)
    setTimeout(() => setResult(null), 4000)
  }

  const bg = variant === 'danger'
    ? 'bg-red-800 hover:bg-red-700 border-red-700'
    : variant === 'warn'
      ? 'bg-yellow-800 hover:bg-yellow-700 border-yellow-700'
      : 'bg-gray-800 hover:bg-gray-700 border-gray-700'

  return (
    <div className="flex items-center gap-2">
      <button onClick={run} disabled={loading}
        className={`px-4 py-2 rounded text-sm border transition-colors disabled:opacity-50 ${bg}`}>
        {loading ? '...' : label}
      </button>
      {result && (
        <span className={`text-xs ${result === 'failed' ? 'text-red-400' : 'text-green-400'}`}>
          {result}
        </span>
      )}
    </div>
  )
}

export default function Commands() {
  const { data: status } = useApi<UpsStatus>('/api/status', 5000)
  const caps = status?.capabilities || []
  const has = (c: string) => caps.includes(c)

  if (!status?.connected) {
    return (
      <div>
        <h2 className="text-xl font-semibold mb-4">Commands</h2>
        <div className="text-center py-12 text-gray-500">
          <p>UPS not connected. Commands unavailable.</p>
        </div>
      </div>
    )
  }

  return (
    <div>
      <h2 className="text-xl font-semibold mb-4">Commands</h2>

      <div className="space-y-4">
        <Section title="Testing">
          <div className="flex flex-wrap gap-3">
            {has('battery_test') && <CmdButton label="Battery Test" action="battery_test" />}
            {has('runtime_cal') && (
              <CmdButton label="Runtime Calibration" action="runtime_cal" variant="warn"
                confirm="Runtime calibration deeply discharges the battery. Continue?" />
            )}
            {has('beep') && <CmdButton label="Short Beep" action="beep_short" />}
            {has('beep') && <CmdButton label="Continuous Test" action="beep_continuous" variant="warn"
              confirm="This starts a continuous beep + LED test. Use Mute to stop it." />}
          </div>
        </Section>

        <Section title="Alarms">
          <div className="flex flex-wrap gap-3">
            {has('mute') && <CmdButton label="Mute" action="mute" />}
            {has('mute') && <CmdButton label="Unmute" action="unmute" />}
          </div>
        </Section>

        {has('bypass') && (
          <Section title="Bypass">
            <div className="flex flex-wrap gap-3">
              <CmdButton label="Enable Bypass" action="bypass_on" variant="warn"
                confirm="Enabling bypass removes battery protection. Continue?" />
              <CmdButton label="Disable Bypass" action="bypass_off" />
            </div>
          </Section>
        )}

        {has('clear_faults') && (
          <Section title="Faults">
            <CmdButton label="Clear Faults" action="clear_faults" />
          </Section>
        )}

        <Section title="Shutdown">
          <div className="flex flex-wrap gap-3">
            <CmdButton label="Dry Run" action="shutdown" extra={{ dry_run: true }} />
            <CmdButton label="Shutdown NOW" action="shutdown" variant="danger"
              confirm="This will execute the full shutdown workflow. All configured hosts will be shut down. Are you absolutely sure?" />
          </div>
        </Section>
      </div>
    </div>
  )
}

function Section({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <div className="rounded-lg bg-gray-900 border border-gray-800">
      <div className="px-4 py-2.5 border-b border-gray-800">
        <h3 className="text-xs font-medium text-gray-400 uppercase tracking-wider">{title}</h3>
      </div>
      <div className="px-4 py-3">{children}</div>
    </div>
  )
}
