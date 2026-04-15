import { useApi } from '../hooks/useApi'

interface UpsStatus {
  driver: string
  connected: boolean
  message?: string
  inventory?: {
    model: string
    serial: string
    firmware: string
    nominal_va: number
    nominal_watts: number
  }
  status?: { raw: number; text: string }
  battery?: { charge_pct: number; voltage: number; runtime_sec: number }
  output?: { voltage: number; frequency: number; current: number; load_pct: number }
  bypass?: { voltage: number; frequency: number }
  input?: { voltage: number }
  efficiency?: number
  transfer_reason?: string
  capabilities?: string[]
  he_mode?: { inhibited: boolean; source: string }
}

function Metric({ label, value, unit, warn }: {
  label: string; value: string; unit?: string; warn?: boolean
}) {
  return (
    <div className="flex justify-between items-baseline py-1.5 border-b border-gray-800 last:border-0">
      <span className="text-sm text-gray-400">{label}</span>
      <span className={`text-sm font-mono ${warn ? 'text-yellow-400' : 'text-gray-200'}`}>
        {value}{unit && <span className="text-gray-500 ml-0.5">{unit}</span>}
      </span>
    </div>
  )
}

function Card({ title, children, className = '' }: {
  title: string; children: React.ReactNode; className?: string
}) {
  return (
    <div className={`rounded-lg bg-gray-900 border border-gray-800 ${className}`}>
      <div className="px-4 py-2.5 border-b border-gray-800">
        <h3 className="text-xs font-medium text-gray-400 uppercase tracking-wider">{title}</h3>
      </div>
      <div className="px-4 py-3">{children}</div>
    </div>
  )
}

function StatusBadge({ text }: { text: string }) {
  const isOnline = text.includes('Online')
  const isOnBattery = text.includes('OnBattery')
  const isFault = text.includes('Fault')
  const bg = isFault ? 'bg-red-500' : isOnBattery ? 'bg-yellow-500' : isOnline ? 'bg-green-500' : 'bg-gray-500'
  return (
    <div className="flex items-center gap-2">
      <span className={`w-3 h-3 rounded-full ${bg} animate-pulse`} />
      <span className="text-xl font-semibold">{text}</span>
    </div>
  )
}

const fmtRuntime = (sec: number) => {
  const h = Math.floor(sec / 3600)
  const m = Math.floor((sec % 3600) / 60)
  return h > 0 ? `${h}h ${m}m` : `${m}m`
}

export default function Dashboard() {
  const { data: status, error } = useApi<UpsStatus>('/api/status', 2000)

  if (error) {
    return (
      <div className="rounded-lg bg-red-900/30 border border-red-800 p-6 text-center">
        <p className="text-red-300 text-lg mb-1">Connection Lost</p>
        <p className="text-red-400/70 text-sm">{error}</p>
      </div>
    )
  }

  if (!status) return <LoadingPulse />

  if (!status.connected) {
    return (
      <div className="rounded-lg bg-yellow-900/30 border border-yellow-800 p-6 text-center">
        <p className="text-yellow-200 text-lg mb-1">No UPS Connected</p>
        <p className="text-yellow-400/70 text-sm">{status.message}</p>
      </div>
    )
  }

  const { battery: bat, output: out, input: inp } = status

  return (
    <div>
      <div className="flex items-center justify-between mb-6">
        <StatusBadge text={status.status?.text || 'Unknown'} />
        {status.he_mode?.inhibited && (
          <span className="px-2.5 py-1 rounded bg-yellow-900/50 border border-yellow-700 text-xs text-yellow-300">
            HE inhibited: {status.he_mode.source}
          </span>
        )}
      </div>

      <div className="grid grid-cols-1 md:grid-cols-2 xl:grid-cols-4 gap-4 mb-6">
        <Card title="Battery">
          <p className={`text-3xl font-bold mb-1 ${
            bat && bat.charge_pct < 30 ? 'text-red-400' : bat && bat.charge_pct < 60 ? 'text-yellow-400' : 'text-green-400'
          }`}>{bat?.charge_pct.toFixed(0)}%</p>
          <p className="text-sm text-gray-400">{bat ? fmtRuntime(bat.runtime_sec) : '--'} remaining</p>
          <div className="mt-3">
            <Metric label="Voltage" value={bat?.voltage.toFixed(1) || '--'} unit="V" />
          </div>
        </Card>

        <Card title="Load">
          <p className={`text-3xl font-bold mb-1 ${
            out && out.load_pct > 80 ? 'text-red-400' : out && out.load_pct > 60 ? 'text-yellow-400' : 'text-white'
          }`}>{out?.load_pct.toFixed(1)}%</p>
          <p className="text-sm text-gray-400">{out?.current.toFixed(1)}A draw</p>
        </Card>

        <Card title="Output">
          <Metric label="Voltage" value={out?.voltage.toFixed(1) || '--'} unit="V" />
          <Metric label="Frequency" value={out?.frequency.toFixed(1) || '--'} unit="Hz" />
        </Card>

        <Card title="Input / Bypass">
          <Metric label="Input Voltage" value={inp?.voltage.toFixed(1) || '--'} unit="V" />
          <Metric label="Bypass Voltage" value={status.bypass?.voltage.toFixed(1) || '--'} unit="V" />
          <Metric label="Bypass Frequency" value={status.bypass?.frequency.toFixed(1) || '--'} unit="Hz" />
          <Metric label="Transfer Reason" value={status.transfer_reason || '--'} />
        </Card>
      </div>

      <div className="grid grid-cols-1 lg:grid-cols-2 gap-4">
        {status.inventory && (
          <Card title="Inventory">
            <Metric label="Model" value={status.inventory.model.trim()} />
            <Metric label="Serial" value={status.inventory.serial.trim()} />
            <Metric label="Rating" value={`${status.inventory.nominal_va} / ${status.inventory.nominal_watts}`} unit="VA / W" />
            <Metric label="Driver" value={status.driver} />
          </Card>
        )}

        <Card title="Capabilities">
          <div className="flex flex-wrap gap-1.5 py-1">
            {status.capabilities?.map((cap) => (
              <span key={cap} className="px-2 py-0.5 rounded-full bg-gray-800 text-[11px] text-gray-300 border border-gray-700">
                {cap.replace(/_/g, ' ')}
              </span>
            ))}
          </div>
        </Card>
      </div>
    </div>
  )
}

function LoadingPulse() {
  return (
    <div className="grid grid-cols-1 md:grid-cols-2 xl:grid-cols-4 gap-4">
      {[1,2,3,4].map((i) => (
        <div key={i} className="rounded-lg bg-gray-900 border border-gray-800 h-32 animate-pulse" />
      ))}
    </div>
  )
}
