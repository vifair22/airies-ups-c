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
  input?: { voltage: number }
  efficiency?: number
  transfer_reason?: string
  capabilities?: string[]
  he_mode?: { inhibited: boolean; source: string }
}

function Card({ title, value, sub, color }: {
  title: string; value: string; sub?: string; color?: 'green' | 'red' | 'yellow'
}) {
  const c = color === 'green' ? 'text-green-400'
    : color === 'red' ? 'text-red-400'
    : color === 'yellow' ? 'text-yellow-400'
    : 'text-white'
  return (
    <div className="rounded-lg bg-gray-900 border border-gray-800 p-4">
      <p className="text-sm text-gray-400">{title}</p>
      <p className={`text-2xl font-semibold mt-1 ${c}`}>{value}</p>
      {sub && <p className="text-sm text-gray-500 mt-1">{sub}</p>}
    </div>
  )
}

const fmtRuntime = (sec: number) => `${Math.floor(sec / 60)}m ${sec % 60}s`

export default function Dashboard() {
  const { data: status, error } = useApi<UpsStatus>('/api/status', 2000)

  return (
    <div>
      <h2 className="text-xl font-semibold mb-4">Dashboard</h2>

      {error && (
        <div className="rounded-lg bg-red-900/50 border border-red-700 p-4 mb-4">
          <p className="text-red-200">Cannot connect to daemon: {error}</p>
        </div>
      )}

      {status && !status.connected && (
        <div className="rounded-lg bg-yellow-900/50 border border-yellow-700 p-4 mb-4">
          <p className="text-yellow-200">{status.message || 'UPS not connected'}</p>
        </div>
      )}

      {status?.connected && (
        <>
          <div className="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-4 gap-4 mb-6">
            <Card
              title="Status"
              value={status.status?.text || 'Unknown'}
              color={status.status?.text === 'Online' ? 'green' : 'red'}
            />
            <Card
              title="Battery"
              value={`${status.battery?.charge_pct.toFixed(0)}%`}
              sub={status.battery ? fmtRuntime(status.battery.runtime_sec) : ''}
              color={status.battery && status.battery.charge_pct < 50 ? 'yellow' : undefined}
            />
            <Card
              title="Load"
              value={`${status.output?.load_pct.toFixed(1)}%`}
              sub={`${status.output?.current.toFixed(1)}A`}
              color={status.output && status.output.load_pct > 80 ? 'red' : undefined}
            />
            <Card
              title="Input"
              value={`${status.input?.voltage.toFixed(1)}V`}
              sub={`Out: ${status.output?.voltage.toFixed(1)}V ${status.output?.frequency.toFixed(0)}Hz`}
            />
          </div>

          {status.he_mode?.inhibited && (
            <div className="rounded-lg bg-yellow-900/50 border border-yellow-700 p-3 mb-4 text-sm">
              HE mode inhibited ({status.he_mode.source})
            </div>
          )}

          <div className="grid grid-cols-1 lg:grid-cols-2 gap-4 mb-6">
            {status.inventory && (
              <div className="rounded-lg bg-gray-900 border border-gray-800 p-4">
                <h3 className="text-sm font-medium text-gray-400 mb-2">Inventory</h3>
                <div className="text-sm space-y-1">
                  <p>Model: {status.inventory.model.trim()}</p>
                  <p>Serial: {status.inventory.serial.trim()}</p>
                  <p>Rating: {status.inventory.nominal_va}VA / {status.inventory.nominal_watts}W</p>
                  <p>Driver: {status.driver}</p>
                </div>
              </div>
            )}

            <div className="rounded-lg bg-gray-900 border border-gray-800 p-4">
              <h3 className="text-sm font-medium text-gray-400 mb-2">Details</h3>
              <div className="text-sm space-y-1">
                <p>Battery Voltage: {status.battery?.voltage.toFixed(1)}V</p>
                <p>Transfer Reason: {status.transfer_reason}</p>
                <p>Efficiency: {status.efficiency !== undefined && status.efficiency >= 0
                  ? `${(status.efficiency * 100 / 128).toFixed(1)}%` : 'N/A'}</p>
              </div>
            </div>
          </div>

          {status.capabilities && (
            <div className="rounded-lg bg-gray-900 border border-gray-800 p-4">
              <h3 className="text-sm font-medium text-gray-400 mb-2">Capabilities</h3>
              <div className="flex flex-wrap gap-2">
                {status.capabilities.map((cap) => (
                  <span key={cap} className="px-2 py-1 rounded bg-gray-800 text-xs text-gray-300">
                    {cap}
                  </span>
                ))}
              </div>
            </div>
          )}
        </>
      )}
    </div>
  )
}
