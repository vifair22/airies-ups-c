import { useEffect, useState } from 'react'

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
  status?: {
    raw: number
    text: string
  }
  battery?: {
    charge_pct: number
    voltage: number
    runtime_sec: number
  }
  output?: {
    voltage: number
    frequency: number
    current: number
    load_pct: number
  }
  input?: {
    voltage: number
  }
  efficiency?: number
  transfer_reason?: string
  capabilities?: string[]
}

function App() {
  const [status, setStatus] = useState<UpsStatus | null>(null)
  const [error, setError] = useState<string | null>(null)

  const fetchStatus = async () => {
    try {
      const res = await fetch('/api/status')
      const data = await res.json()
      setStatus(data)
      setError(null)
    } catch (e) {
      setError('Cannot connect to daemon')
    }
  }

  useEffect(() => {
    fetchStatus()
    const interval = setInterval(fetchStatus, 2000)
    return () => clearInterval(interval)
  }, [])

  const fmtRuntime = (sec: number) => {
    const m = Math.floor(sec / 60)
    const s = sec % 60
    return `${m}m ${s}s`
  }

  return (
    <div className="min-h-screen bg-gray-950 text-gray-100">
      <header className="border-b border-gray-800 px-6 py-4">
        <h1 className="text-xl font-semibold">airies-ups</h1>
      </header>

      <main className="p-6">
        {error && (
          <div className="rounded-lg bg-red-900/50 border border-red-700 p-4 mb-6">
            <p className="text-red-200">{error}</p>
          </div>
        )}

        {status && !status.connected && (
          <div className="rounded-lg bg-yellow-900/50 border border-yellow-700 p-4 mb-6">
            <p className="text-yellow-200">{status.message || 'UPS not connected'}</p>
            <p className="text-yellow-400 text-sm mt-1">Driver: {status.driver}</p>
          </div>
        )}

        {status?.connected && (
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
            />
            <Card
              title="Load"
              value={`${status.output?.load_pct.toFixed(1)}%`}
              sub={`${status.output?.current.toFixed(1)}A`}
            />
            <Card
              title="Input"
              value={`${status.input?.voltage.toFixed(1)}V`}
              sub={`Out: ${status.output?.voltage.toFixed(1)}V ${status.output?.frequency.toFixed(0)}Hz`}
            />
          </div>
        )}

        {status?.inventory && (
          <div className="rounded-lg bg-gray-900 border border-gray-800 p-4 mb-6">
            <h2 className="text-sm font-medium text-gray-400 mb-2">Inventory</h2>
            <p className="text-sm">
              {status.inventory.model.trim()} | {status.inventory.serial.trim()} |{' '}
              {status.inventory.nominal_va}VA / {status.inventory.nominal_watts}W |{' '}
              Driver: {status.driver}
            </p>
          </div>
        )}

        {status?.capabilities && (
          <div className="rounded-lg bg-gray-900 border border-gray-800 p-4">
            <h2 className="text-sm font-medium text-gray-400 mb-2">Capabilities</h2>
            <div className="flex flex-wrap gap-2">
              {status.capabilities.map((cap) => (
                <span key={cap} className="px-2 py-1 rounded bg-gray-800 text-xs text-gray-300">
                  {cap}
                </span>
              ))}
            </div>
          </div>
        )}
      </main>
    </div>
  )
}

function Card({ title, value, sub, color }: {
  title: string
  value: string
  sub?: string
  color?: 'green' | 'red'
}) {
  const colorClass = color === 'green'
    ? 'text-green-400'
    : color === 'red'
      ? 'text-red-400'
      : 'text-white'

  return (
    <div className="rounded-lg bg-gray-900 border border-gray-800 p-4">
      <p className="text-sm text-gray-400">{title}</p>
      <p className={`text-2xl font-semibold mt-1 ${colorClass}`}>{value}</p>
      {sub && <p className="text-sm text-gray-500 mt-1">{sub}</p>}
    </div>
  )
}

export default App
