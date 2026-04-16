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
  { key: 'runtime_sec', label: 'Runtime', unit: 'min', color: '#14b8a6' },
  { key: 'efficiency', label: 'Efficiency', unit: '', color: '#a855f7',
    format: (v) => v >= 0 ? (v * 100 / 128).toFixed(1) + '%' : 'N/A' },
]

const presets = [
  { label: '5m', min: 5 },
  { label: '15m', min: 15 },
  { label: '30m', min: 30 },
  { label: '1h', min: 60 },
  { label: '2h', min: 120 },
  { label: '6h', min: 360 },
  { label: '12h', min: 720 },
  { label: '1d', min: 1440 },
  { label: '2d', min: 2880 },
  { label: '5d', min: 7200 },
  { label: '7d', min: 10080 },
  { label: '15d', min: 21600 },
  { label: '30d', min: 43200 },
  { label: '60d', min: 86400 },
  { label: '90d', min: 129600 },
]

function tsToMs(ts: string): number {
  return new Date(ts + 'Z').getTime()
}

export default function Telemetry() {
  const [activePreset, setActivePreset] = useState(60) /* default 1h */
  const [fromDate, setFromDate] = useState(() => {
    const from = new Date(Date.now() - 60 * 60000)
    return from.toISOString().replace('T', ' ').slice(0, 19)
  })
  const [toDate, setToDate] = useState('')
  const [expanded, setExpanded] = useState<string | null>(null)

  const url = `/api/telemetry?from=${encodeURIComponent(fromDate)}${toDate ? `&to=${encodeURIComponent(toDate)}` : ''}&limit=5000`
  const { data, error, loading } = useApi<TelemetryPoint[]>(url, 30000)

  const setPreset = useCallback((minutes: number) => {
    const from = new Date(Date.now() - minutes * 60000)
    setFromDate(from.toISOString().replace('T', ' ').slice(0, 19))
    setToDate('')
    setActivePreset(minutes)
  }, [])

  const setCustomRange = useCallback((from: string, to: string) => {
    setFromDate(from)
    setToDate(to)
    setActivePreset(0)
  }, [])

  /* Time window for absolute scale */
  const windowStartMs = tsToMs(fromDate)
  const windowEndMs = toDate ? tsToMs(toDate) : Date.now()
  const windowMs = windowEndMs - windowStartMs

  if (error) return <p className="text-red-400">{error}</p>

  const points = data ? (
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
          <input type="datetime-local" value={fromDate.replace(' ', 'T').slice(0, 16)}
            onChange={(e) => setCustomRange(e.target.value.replace('T', ' ') + ':00', toDate)}
            className="bg-gray-800 border border-gray-700 rounded px-2 py-1 text-xs" />
        </div>
        <div className="flex items-center gap-1.5">
          <label className="text-xs text-gray-400">To</label>
          <input type="datetime-local" value={(toDate || new Date().toISOString().slice(0, 16)).replace(' ', 'T').slice(0, 16)}
            onChange={(e) => setCustomRange(fromDate, e.target.value.replace('T', ' ') + ':00')}
            className="bg-gray-800 border border-gray-700 rounded px-2 py-1 text-xs" />
        </div>
        <div className="flex gap-1 flex-wrap">
          {presets.map(({ label, min }) => (
            <button key={label} onClick={() => setPreset(min)}
              className={`px-2 py-1 text-xs rounded border transition-colors ${
                activePreset === min
                  ? 'bg-blue-900/60 border-blue-600 text-blue-300'
                  : 'bg-gray-800 hover:bg-gray-700 border-gray-700'
              }`}>
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
          <p className="text-sm">No data in the selected time range.</p>
        </div>
      )}

      {!loading && (
        <div className="grid grid-cols-1 lg:grid-cols-2 gap-4">
          {metrics.map((m) => (
            <Chart
              key={m.key}
              metric={m}
              points={points}
              windowStartMs={windowStartMs}
              windowMs={windowMs}
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
                  <Chart metric={m} points={points} windowStartMs={windowStartMs} windowMs={windowMs} expanded />
                </>
              )
            })()}
          </div>
        </div>
      )}
    </div>
  )
}

function fmtTimeLabel(ms: number, windowMs: number): string {
  const d = new Date(ms)
  if (windowMs > 2 * 86400000) /* >2 days: show date */
    return `${d.getUTCMonth()+1}/${d.getUTCDate()}`
  if (windowMs > 86400000) /* >1 day: show date + time */
    return `${d.getUTCMonth()+1}/${d.getUTCDate()} ${String(d.getUTCHours()).padStart(2,'0')}:${String(d.getUTCMinutes()).padStart(2,'0')}`
  return `${String(d.getUTCHours()).padStart(2,'0')}:${String(d.getUTCMinutes()).padStart(2,'0')}`
}

function Chart({ metric, points, windowStartMs, windowMs, expanded, onClick }: {
  metric: MetricDef
  points: TelemetryPoint[]
  windowStartMs: number
  windowMs: number
  expanded?: boolean
  onClick?: () => void
}) {
  const getValue = (p: TelemetryPoint) => {
    const v = p[metric.key] as number
    if (metric.key === 'runtime_sec') return v / 60
    return v
  }

  if (points.length === 0) {
    return (
      <div className={`rounded-lg bg-gray-900 border border-gray-800 p-4 ${!expanded ? 'cursor-pointer hover:border-gray-700 transition-colors' : ''}`}
        onClick={!expanded ? onClick : undefined}>
        <div className="flex items-baseline justify-between mb-2">
          <h3 className="text-sm text-gray-400">{metric.label}</h3>
          <span className="text-xs text-gray-600">No data</span>
        </div>
        <svg viewBox={`0 0 400 80`} className="w-full h-20" preserveAspectRatio="none">
          <line x1="0" y1="40" x2="400" y2="40" stroke="#374151" strokeWidth="0.5" strokeDasharray="4" />
        </svg>
        <div className="flex justify-between mt-2">
          <span className="text-[10px] text-gray-600 font-mono">{fmtTimeLabel(windowStartMs, windowMs)}</span>
          <span className="text-[10px] text-gray-600 font-mono">{fmtTimeLabel(windowStartMs + windowMs, windowMs)}</span>
        </div>
      </div>
    )
  }

  const values = points.map(getValue)
  const timestamps = points.map(p => tsToMs(p.timestamp))
  const min = Math.min(...values)
  const max = Math.max(...values)
  const current = values[values.length - 1]
  const avg = values.reduce((a, b) => a + b, 0) / values.length
  const vRange = max - min || 1

  const w = expanded ? 800 : 400
  const h = expanded ? 200 : 80
  const pad = 4

  /* Absolute time scale: X position based on timestamp within window */
  const pathPoints = timestamps.map((ts, i) => {
    const x = ((ts - windowStartMs) / windowMs) * w
    const y = h - pad - ((values[i] - min) / vRange) * (h - pad * 2)
    return `${x},${y}`
  })

  const areaD = `M ${pathPoints[0].split(',')[0]},${h} L ${pathPoints.join(' L ')} L ${pathPoints[pathPoints.length-1].split(',')[0]},${h} Z`
  const lineD = `M ${pathPoints.join(' L ')}`

  const fmtVal = metric.format || ((v: number) => v.toFixed(1))

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
        <path d={lineD} fill="none" stroke={metric.color} strokeWidth={expanded ? '0.8' : '1'} />
      </svg>

      <div className="flex justify-between items-center mt-2">
        <span className="text-[10px] text-gray-600 font-mono">
          {fmtTimeLabel(windowStartMs, windowMs)}
        </span>
        <div className="flex gap-3 text-[10px] text-gray-600">
          <span>min: {fmtVal(min)}</span>
          <span>avg: {fmtVal(avg)}</span>
          <span>max: {fmtVal(max)}</span>
        </div>
        <span className="text-[10px] text-gray-600 font-mono">
          {fmtTimeLabel(windowStartMs + windowMs, windowMs)}
        </span>
      </div>
    </div>
  )
}
