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

  if (loading) return (
    <div>
      <h2 className="text-xl font-semibold mb-4">Telemetry</h2>
      <div className="grid grid-cols-1 lg:grid-cols-2 gap-4">
        {[1,2,3,4].map(i => (
          <div key={i} className="rounded-lg bg-gray-900 border border-gray-800 h-40 animate-pulse" />
        ))}
      </div>
    </div>
  )
  if (error) return <p className="text-red-400">{error}</p>
  if (!data || data.length === 0) return (
    <div>
      <h2 className="text-xl font-semibold mb-4">Telemetry</h2>
      <div className="text-center py-12 text-gray-500">
        <p className="text-lg mb-1">No telemetry data yet</p>
        <p className="text-sm">Data is recorded every 30 seconds when the UPS is connected.</p>
      </div>
    </div>
  )

  const points = [...data].reverse()

  return (
    <div>
      <div className="flex items-center justify-between mb-4">
        <h2 className="text-xl font-semibold">Telemetry</h2>
        <span className="text-xs text-gray-500">{points.length} samples</span>
      </div>

      <div className="grid grid-cols-1 lg:grid-cols-2 gap-4">
        <Chart title="Battery Charge" unit="%" points={points}
          getValue={(p) => p.charge_pct} color="#22c55e" />
        <Chart title="Load" unit="%" points={points}
          getValue={(p) => p.load_pct} color="#f59e0b" />
        <Chart title="Input Voltage" unit="V" points={points}
          getValue={(p) => p.input_voltage} color="#3b82f6" />
        <Chart title="Output Voltage" unit="V" points={points}
          getValue={(p) => p.output_voltage} color="#8b5cf6" />
        <Chart title="Battery Voltage" unit="V" points={points}
          getValue={(p) => p.battery_voltage} color="#ef4444" />
        <Chart title="Runtime" unit="min" points={points}
          getValue={(p) => p.runtime_sec / 60} color="#06b6d4" />
      </div>
    </div>
  )
}

function Chart({ title, unit, points, getValue, color }: {
  title: string
  unit: string
  points: TelemetryPoint[]
  getValue: (p: TelemetryPoint) => number
  color: string
}) {
  const values = points.map(getValue)
  const min = Math.min(...values)
  const max = Math.max(...values)
  const current = values[values.length - 1]
  const range = max - min || 1

  const w = 400
  const h = 80
  const pad = 2
  const pathPoints = values.map((v, i) => {
    const x = (i / (values.length - 1 || 1)) * w
    const y = h - pad - ((v - min) / range) * (h - pad * 2)
    return `${x},${y}`
  })

  // Create filled area
  const areaD = `M 0,${h} L ${pathPoints.join(' L ')} L ${w},${h} Z`
  const lineD = `M ${pathPoints.join(' L ')}`

  return (
    <div className="rounded-lg bg-gray-900 border border-gray-800 p-4">
      <div className="flex items-baseline justify-between mb-3">
        <h3 className="text-sm text-gray-400">{title}</h3>
        <div className="text-right">
          <span className="text-xl font-semibold font-mono" style={{ color }}>
            {current.toFixed(1)}
          </span>
          <span className="text-xs text-gray-500 ml-1">{unit}</span>
        </div>
      </div>

      <svg viewBox={`0 0 ${w} ${h}`} className="w-full h-20" preserveAspectRatio="none">
        <path d={areaD} fill={color} opacity="0.1" />
        <path d={lineD} fill="none" stroke={color} strokeWidth="1.5" />
      </svg>

      <div className="flex justify-between items-center mt-2">
        <span className="text-[10px] text-gray-600 font-mono">
          {points[0]?.timestamp.split(' ')[1]?.slice(0, 5) || ''}
        </span>
        <div className="flex gap-3 text-[10px] text-gray-600">
          <span>min: {min.toFixed(1)}</span>
          <span>max: {max.toFixed(1)}</span>
        </div>
        <span className="text-[10px] text-gray-600 font-mono">
          {points[points.length - 1]?.timestamp.split(' ')[1]?.slice(0, 5) || ''}
        </span>
      </div>
    </div>
  )
}
