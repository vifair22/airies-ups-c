import { useState, useCallback } from 'react'
import { useApi } from '../hooks/useApi'

interface TelemetryPoint {
  timestamp: string
  charge_pct: number
  load_pct: number
  input_voltage: number
  output_voltage: number
  battery_voltage: number
  output_frequency: number
  output_current: number
  runtime_sec: number
  efficiency: number
}

interface MetricDef {
  key: keyof TelemetryPoint
  label: string
  unit: string
  color: string
  format?: (v: number) => string
}

const metrics: MetricDef[] = [
  { key: 'charge_pct', label: 'Battery Charge', unit: '%', color: '#22c55e' },
  { key: 'load_pct', label: 'Load', unit: '%', color: '#f59e0b' },
  { key: 'input_voltage', label: 'Input Voltage', unit: 'V', color: '#3b82f6' },
  { key: 'output_voltage', label: 'Output Voltage', unit: 'V', color: '#8b5cf6' },
  { key: 'battery_voltage', label: 'Battery Voltage', unit: 'V', color: '#ef4444' },
  { key: 'output_frequency', label: 'Output Frequency', unit: 'Hz', color: '#06b6d4' },
  { key: 'output_current', label: 'Output Current', unit: 'A', color: '#f97316' },
  { key: 'runtime_sec', label: 'Runtime', unit: 'min', color: '#14b8a6',
    format: (v) => (v / 60).toFixed(1) },
  { key: 'efficiency', label: 'Efficiency', unit: '', color: '#a855f7',
    format: (v) => v >= 0 ? (v * 100 / 128).toFixed(1) + '%' : 'N/A' },
]

function todayStr() {
  const d = new Date()
  return d.toISOString().split('T')[0]
}

function daysAgoStr(n: number) {
  const d = new Date()
  d.setDate(d.getDate() - n)
  return d.toISOString().split('T')[0]
}

export default function Telemetry() {
  const [fromDate, setFromDate] = useState('')
  const [toDate, setToDate] = useState('')
  const [expanded, setExpanded] = useState<string | null>(null)

  // No date filter = latest 500 points. With dates = filtered query.
  const url = fromDate
    ? `/api/telemetry?from=${fromDate}&to=${toDate || todayStr()} 23:59:59&limit=2000`
    : '/api/telemetry?limit=500'
  const { data, error, loading } = useApi<TelemetryPoint[]>(url, 30000)

  const setPreset = useCallback((days: number) => {
    if (days === 0) {
      setFromDate('')
      setToDate('')
    } else {
      setFromDate(daysAgoStr(days))
      setToDate(todayStr())
    }
  }, [])

  if (error) return <p className="text-red-400">{error}</p>

  const points = data ? (
    // API returns DESC for default, ASC for date-filtered — ensure chronological
    data[0] && data[data.length-1] &&
    data[0].timestamp > data[data.length-1].timestamp
      ? [...data].reverse() : data
  ) : []

  return (
    <div>
      <div className="flex items-center justify-between mb-4">
        <h2 className="text-xl font-semibold">Telemetry</h2>
        {points.length > 0 && (
          <span className="text-xs text-gray-500">{points.length} samples</span>
        )}
      </div>

      {/* Date controls */}
      <div className="flex items-center gap-3 mb-4 flex-wrap">
        <div className="flex items-center gap-1.5">
          <label className="text-xs text-gray-400">From</label>
          <input type="date" value={fromDate} onChange={(e) => setFromDate(e.target.value)}
            className="bg-gray-800 border border-gray-700 rounded px-2 py-1 text-xs" />
        </div>
        <div className="flex items-center gap-1.5">
          <label className="text-xs text-gray-400">To</label>
          <input type="date" value={toDate} onChange={(e) => setToDate(e.target.value)}
            className="bg-gray-800 border border-gray-700 rounded px-2 py-1 text-xs" />
        </div>
        <div className="flex gap-1">
          {[
            { label: 'Latest', days: 0 },
            { label: '7d', days: 7 },
            { label: '30d', days: 30 },
            { label: '90d', days: 90 },
          ].map(({ label, days }) => (
            <button key={label} onClick={() => setPreset(days)}
              className="px-2 py-1 text-xs rounded bg-gray-800 hover:bg-gray-700 border border-gray-700 transition-colors">
              {label}
            </button>
          ))}
        </div>
      </div>

      {loading && (
        <div className="grid grid-cols-1 lg:grid-cols-2 gap-4">
          {[1,2,3,4].map(i => (
            <div key={i} className="rounded-lg bg-gray-900 border border-gray-800 h-40 animate-pulse" />
          ))}
        </div>
      )}

      {!loading && points.length === 0 && (
        <div className="text-center py-12 text-gray-500">
          <p className="text-lg mb-1">No telemetry data</p>
          <p className="text-sm">No data in the selected date range. Data is recorded every 30 seconds.</p>
        </div>
      )}

      {!loading && points.length > 0 && (
        <div className="grid grid-cols-1 lg:grid-cols-2 gap-4">
          {metrics.map((m) => (
            <Chart
              key={m.key}
              metric={m}
              points={points}
              onClick={() => setExpanded(m.key)}
            />
          ))}
        </div>
      )}

      {/* Expanded modal */}
      {expanded && points.length > 0 && (
        <div className="fixed inset-0 bg-black/70 z-50 flex items-center justify-center p-6"
          onClick={() => setExpanded(null)}>
          <div className="bg-gray-900 border border-gray-700 rounded-xl w-full max-w-4xl p-6"
            onClick={(e) => e.stopPropagation()}>
            {(() => {
              const m = metrics.find(m => m.key === expanded)!
              return (
                <>
                  <div className="flex items-center justify-between mb-4">
                    <h3 className="text-lg font-semibold">{m.label}</h3>
                    <button onClick={() => setExpanded(null)}
                      className="text-gray-500 hover:text-gray-300 text-xl">&times;</button>
                  </div>
                  <Chart metric={m} points={points} expanded />
                </>
              )
            })()}
          </div>
        </div>
      )}
    </div>
  )
}

function Chart({ metric, points, expanded, onClick }: {
  metric: MetricDef
  points: TelemetryPoint[]
  expanded?: boolean
  onClick?: () => void
}) {
  const getValue = (p: TelemetryPoint) => {
    const v = p[metric.key] as number
    if (metric.key === 'runtime_sec') return v / 60
    return v
  }

  const values = points.map(getValue)
  const min = Math.min(...values)
  const max = Math.max(...values)
  const current = values[values.length - 1]
  const avg = values.reduce((a, b) => a + b, 0) / values.length
  const range = max - min || 1

  const w = expanded ? 800 : 400
  const h = expanded ? 200 : 80
  const pad = 4

  const pathPoints = values.map((v, i) => {
    const x = (i / (values.length - 1 || 1)) * w
    const y = h - pad - ((v - min) / range) * (h - pad * 2)
    return `${x},${y}`
  })

  const areaD = `M 0,${h} L ${pathPoints.join(' L ')} L ${w},${h} Z`
  const lineD = `M ${pathPoints.join(' L ')}`

  const fmtVal = metric.format || ((v: number) => v.toFixed(1))
  const fmtTime = (ts: string) => ts.split(' ')[1]?.slice(0, 5) || ts.slice(5, 10)

  return (
    <div
      className={`rounded-lg bg-gray-900 border border-gray-800 p-4 ${!expanded ? 'cursor-pointer hover:border-gray-700 transition-colors' : ''}`}
      onClick={!expanded ? onClick : undefined}
    >
      <div className="flex items-baseline justify-between mb-2">
        <h3 className="text-sm text-gray-400">{metric.label}</h3>
        <div className="text-right">
          <span className="text-xl font-semibold font-mono" style={{ color: metric.color }}>
            {fmtVal(current)}
          </span>
          <span className="text-xs text-gray-500 ml-1">{metric.unit}</span>
        </div>
      </div>

      <svg viewBox={`0 0 ${w} ${h}`} className={expanded ? 'w-full h-52' : 'w-full h-20'}
        preserveAspectRatio="none">
        <path d={areaD} fill={metric.color} opacity="0.08" />
        <path d={lineD} fill="none" stroke={metric.color} strokeWidth={expanded ? '1' : '1.5'} />
      </svg>

      <div className="flex justify-between items-center mt-2">
        <span className="text-[10px] text-gray-600 font-mono">
          {fmtTime(points[0]?.timestamp || '')}
        </span>
        <div className="flex gap-3 text-[10px] text-gray-600">
          <span>min: {fmtVal(min)}</span>
          <span>avg: {fmtVal(avg)}</span>
          <span>max: {fmtVal(max)}</span>
        </div>
        <span className="text-[10px] text-gray-600 font-mono">
          {fmtTime(points[points.length - 1]?.timestamp || '')}
        </span>
      </div>
    </div>
  )
}
