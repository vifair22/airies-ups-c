import { useApi } from '../hooks/useApi'

interface TelemetryPoint {
  timestamp: string
  charge_pct: number
  load_pct: number
  input_voltage: number
  output_voltage: number
  battery_voltage: number
  output_frequency: number
  runtime_sec: number
}

export default function Telemetry() {
  const { data, error, loading } = useApi<TelemetryPoint[]>('/api/telemetry', 10000)

  if (loading) return <p className="text-gray-500">Loading...</p>
  if (error) return <p className="text-red-400">{error}</p>
  if (!data || data.length === 0) return (
    <div>
      <h2 className="text-xl font-semibold mb-4">Telemetry</h2>
      <p className="text-gray-500">No telemetry data yet. Data is recorded every 30 seconds.</p>
    </div>
  )

  // Reverse so oldest is first (API returns newest first)
  const points = [...data].reverse()

  return (
    <div>
      <h2 className="text-xl font-semibold mb-4">Telemetry</h2>
      <p className="text-sm text-gray-500 mb-4">{points.length} data points</p>

      <div className="grid grid-cols-1 lg:grid-cols-2 gap-4">
        <MetricCard title="Battery Charge (%)" points={points}
          getValue={(p) => p.charge_pct} color="#22c55e" />
        <MetricCard title="Load (%)" points={points}
          getValue={(p) => p.load_pct} color="#f59e0b" />
        <MetricCard title="Input Voltage (V)" points={points}
          getValue={(p) => p.input_voltage} color="#3b82f6" />
        <MetricCard title="Output Voltage (V)" points={points}
          getValue={(p) => p.output_voltage} color="#8b5cf6" />
        <MetricCard title="Battery Voltage (V)" points={points}
          getValue={(p) => p.battery_voltage} color="#ef4444" />
        <MetricCard title="Runtime (min)" points={points}
          getValue={(p) => p.runtime_sec / 60} color="#06b6d4" />
      </div>
    </div>
  )
}

function MetricCard({ title, points, getValue, color }: {
  title: string
  points: TelemetryPoint[]
  getValue: (p: TelemetryPoint) => number
  color: string
}) {
  const values = points.map(getValue)
  const min = Math.min(...values)
  const max = Math.max(...values)
  const current = values[values.length - 1]
  const range = max - min || 1

  // Simple SVG sparkline
  const w = 400
  const h = 60
  const pathPoints = values.map((v, i) => {
    const x = (i / (values.length - 1 || 1)) * w
    const y = h - ((v - min) / range) * (h - 4) - 2
    return `${x},${y}`
  })
  const pathD = `M ${pathPoints.join(' L ')}`

  return (
    <div className="rounded-lg bg-gray-900 border border-gray-800 p-4">
      <div className="flex justify-between items-baseline mb-2">
        <h3 className="text-sm text-gray-400">{title}</h3>
        <span className="text-lg font-semibold" style={{ color }}>{current.toFixed(1)}</span>
      </div>
      <svg viewBox={`0 0 ${w} ${h}`} className="w-full h-16">
        <path d={pathD} fill="none" stroke={color} strokeWidth="2" />
      </svg>
      <div className="flex justify-between text-xs text-gray-600 mt-1">
        <span>{points[0]?.timestamp.split(' ')[1] || ''}</span>
        <span>{points[points.length - 1]?.timestamp.split(' ')[1] || ''}</span>
      </div>
    </div>
  )
}
