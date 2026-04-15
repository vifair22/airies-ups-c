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

function CmdButton({ label, action, extra, confirm: needsConfirm, color = 'gray' }: {
  label: string
  action: string
  extra?: Record<string, unknown>
  confirm?: string
  color?: 'gray' | 'red' | 'yellow'
}) {
  const [result, setResult] = useState<string | null>(null)

  const run = async () => {
    if (needsConfirm && !window.confirm(needsConfirm)) return
    try {
      const res = await apiPost<CmdResult>('/api/cmd', { action, ...extra })
      setResult(res.result || res.error || 'done')
    } catch {
      setResult('failed')
    }
    setTimeout(() => setResult(null), 3000)
  }

  const bg = color === 'red' ? 'bg-red-800 hover:bg-red-700'
    : color === 'yellow' ? 'bg-yellow-800 hover:bg-yellow-700'
    : 'bg-gray-800 hover:bg-gray-700'

  return (
    <div>
      <button onClick={run} className={`px-4 py-2 rounded text-sm ${bg} transition-colors`}>
        {label}
      </button>
      {result && <p className="text-xs text-gray-400 mt-1">{result}</p>}
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
        <p className="text-gray-500">UPS not connected.</p>
      </div>
    )
  }

  return (
    <div>
      <h2 className="text-xl font-semibold mb-4">Commands</h2>

      <div className="space-y-6">
        <Section title="Testing">
          {has('battery_test') && <CmdButton label="Battery Test" action="battery_test" />}
          {has('runtime_cal') && (
            <CmdButton label="Runtime Calibration" action="runtime_cal" color="yellow"
              confirm="Runtime calibration deeply discharges the battery. Continue?" />
          )}
          {has('beep') && <CmdButton label="Beep Test" action="beep" />}
        </Section>

        <Section title="Alarms">
          {has('mute') && <CmdButton label="Mute Alarms" action="mute" />}
          {has('mute') && <CmdButton label="Unmute Alarms" action="unmute" />}
        </Section>

        {has('bypass') && (
          <Section title="Bypass">
            <CmdButton label="Enable Bypass" action="bypass_on" color="yellow"
              confirm="Enabling bypass removes battery protection. Continue?" />
            <CmdButton label="Disable Bypass" action="bypass_off" />
          </Section>
        )}

        {has('clear_faults') && (
          <Section title="Faults">
            <CmdButton label="Clear Faults" action="clear_faults" />
          </Section>
        )}

        <Section title="Shutdown">
          <CmdButton label="Shutdown (Dry Run)" action="shutdown"
            extra={{ dry_run: true }} />
          <CmdButton label="Shutdown NOW" action="shutdown" color="red"
            confirm="This will execute the full shutdown workflow. Are you sure?" />
        </Section>
      </div>
    </div>
  )
}

function Section({ title, children }: { title: string; children: React.ReactNode }) {
  return (
    <div className="rounded-lg bg-gray-900 border border-gray-800 p-4">
      <h3 className="text-sm font-medium text-gray-400 mb-3">{title}</h3>
      <div className="flex flex-wrap gap-3">{children}</div>
    </div>
  )
}
