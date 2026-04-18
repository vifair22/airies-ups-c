import { useState, useEffect } from 'react'
import { useApi, apiPost } from '../hooks/useApi'
import { Modal, WideModal } from '../components/Modal'
import { Field } from '../components/Field'
import type { WeatherStatus, WeatherConfigData, WeatherReport } from '../types/weather'
import type { ConfigReg } from '../types/config'

export default function WeatherConfig() {
  const { data: status } = useApi<WeatherStatus>('/api/weather/status', 5000)
  const { data: config, refetch } = useApi<WeatherConfigData>('/api/weather/config')
  const { data: upsRegs } = useApi<ConfigReg[]>('/api/config/ups')
  const [form, setForm] = useState<WeatherConfigData | null>(null)
  const [saving, setSaving] = useState(false)
  const [saved, setSaved] = useState(false)
  const [simReason, setSimReason] = useState('High winds, severe thunderstorm')
  const [simulating, setSimulating] = useState(false)
  const [showSimModal, setShowSimModal] = useState(false)
  const [showReportModal, setShowReportModal] = useState(false)
  const [toast, setToast] = useState('')
  const [report, setReport] = useState<WeatherReport | null>(null)
  const [reportLoading, setReportLoading] = useState(false)

  useEffect(() => {
    if (config && !form) setForm(config)
  }, [config, form])

  const [restarting, setRestarting] = useState(false)

  const save = async () => {
    if (!form) return
    setSaving(true)
    try {
      await apiPost('/api/weather/config', form)
      setSaved(true)
      setRestarting(true)
      await apiPost('/api/restart', {})
      /* Poll until daemon is back */
      const poll = () => {
        fetch('/api/weather/status')
          .then(r => { if (r.ok) { setRestarting(false); refetch() } else setTimeout(poll, 500) })
          .catch(() => setTimeout(poll, 500))
      }
      setTimeout(poll, 1500)
    } catch {
      alert('Failed to save')
      setSaving(false)
    }
  }

  /* Get writable registers for the control register dropdown */
  const writableRegs = upsRegs?.filter(r => r.writable) ?? []

  /* Get options for the selected control register (if it's a bitfield type) */
  const selectedReg = upsRegs?.find(r => r.name === form?.control_register)

  return (
    <div>
      <h2 className="text-xl font-semibold mb-4">Weather</h2>

      {status && (
        <div className={`rounded-lg border p-4 mb-6 ${
          status.severe
            ? 'bg-status-red border-red-600/30'
            : status.enabled
              ? 'bg-status-green border-green-600/30'
              : 'bg-panel border-edge'
        }`}>
          <div className="flex items-center gap-3">
            <span className={`w-3 h-3 rounded-full ${
              status.severe ? 'bg-red-500' : status.enabled ? 'bg-green-500' : 'bg-faint'
            }`} />
            <span className="text-sm font-medium">
              {status.severe ? 'Severe Weather Active' : status.enabled ? 'Monitoring — All Clear' : 'Disabled'}
            </span>
            {status.severe && status.simulated && (
              <span className="text-[10px] px-1.5 py-0.5 rounded bg-status-yellow text-yellow-600">SIMULATED</span>
            )}
          </div>
          {status.severe && status.reasons && (
            <p className="text-sm text-red-600 mt-2">{status.reasons}</p>
          )}
        </div>
      )}

      {/* Action buttons */}
      {status?.enabled && (
        <div className="flex gap-3 mb-6">
          <button onClick={async () => {
              setReportLoading(true)
              try {
                const res = await fetch('/api/weather/report', {
                  headers: { Authorization: `Bearer ${localStorage.getItem('auth_token')}` }
                })
                setReport(await res.json())
                setShowReportModal(true)
              } catch { /* ignore */ }
              setReportLoading(false)
            }}
            disabled={reportLoading}
            className="px-4 py-1.5 bg-accent hover:bg-accent-hover text-white rounded text-sm disabled:opacity-50">
            {reportLoading ? 'Loading...' : 'View Weather Report'}
          </button>
          <button onClick={() => setShowSimModal(true)}
            className="px-4 py-1.5 bg-field hover:bg-field-hover border border-edge-strong rounded text-sm">
            Simulate Event
          </button>
          {status.severe && (
            <button disabled={simulating}
              onClick={async () => {
                setSimulating(true)
                try {
                  await apiPost('/api/weather/simulate', { action: 'clear' })
                  setToast('Simulation cleared — will restore on next poll cycle')
                  setTimeout(() => setToast(''), 4000)
                } catch { /* ignore */ }
                setSimulating(false)
              }}
              className="px-4 py-1.5 bg-field hover:bg-field-hover border border-edge-strong rounded text-sm disabled:opacity-50">
              Clear Simulation
            </button>
          )}
        </div>
      )}

      {toast && (
        <div className="mb-4 px-4 py-2 rounded bg-green-900/30 border border-green-700 text-sm text-green-300">
          {toast}
        </div>
      )}

      {/* Simulate modal */}
      {showSimModal && (
        <Modal onClose={() => setShowSimModal(false)}>
          <h3 className="text-lg font-semibold mb-2">Simulate Severe Weather</h3>
          <p className="text-sm text-muted mb-4">
            Injects a weather event into the next poll cycle. The full severe weather flow will trigger including parameter override.
          </p>
          <div className="mb-4">
            <label className="text-xs text-muted">Reason</label>
            <input type="text" value={simReason} onChange={(e) => setSimReason(e.target.value)}
              className="block w-full bg-field border border-edge-strong rounded px-3 py-2 text-sm mt-1" />
          </div>
          <div className="flex gap-3">
            <button disabled={simulating}
              onClick={async () => {
                setSimulating(true)
                try {
                  await apiPost('/api/weather/simulate', { action: 'severe', reason: simReason })
                  setToast('Severe weather event injected — will trigger on next poll cycle')
                  setTimeout(() => setToast(''), 4000)
                } catch { /* ignore */ }
                setSimulating(false)
                setShowSimModal(false)
              }}
              className="px-4 py-2 bg-red-700 hover:bg-red-600 text-white rounded text-sm disabled:opacity-50">
              {simulating ? 'Injecting...' : 'Trigger Severe'}
            </button>
            <button onClick={() => setShowSimModal(false)}
              className="px-4 py-2 bg-field hover:bg-field-hover border border-edge-strong rounded text-sm">
              Cancel
            </button>
          </div>
        </Modal>
      )}

      {/* Weather report modal */}
      {showReportModal && report && (
        <WideModal onClose={() => setShowReportModal(false)}>
          <div className="flex items-center justify-between mb-4">
            <h3 className="text-lg font-semibold">NWS Weather Report</h3>
            <button onClick={() => setShowReportModal(false)} className="text-muted hover:text-primary text-sm">Close</button>
          </div>

          {/* Active Alerts */}
          <h4 className="text-xs font-medium text-muted uppercase tracking-wider mb-2">Active Alerts</h4>
          {report.alerts.length === 0 ? (
            <p className="text-sm text-muted mb-4">No active alerts for configured zones.</p>
          ) : (
            <div className="space-y-2 mb-4">
              {report.alerts.map((a, i) => (
                <div key={i} className={`rounded border p-3 ${a.matched ? 'border-red-600/30 bg-status-red' : 'border-edge bg-field'}`}>
                  <div className="flex items-center gap-2">
                    <span className="text-sm font-medium">{a.event}</span>
                    {a.matched && <span className="text-[10px] px-1.5 py-0.5 rounded bg-status-red text-red-600 border border-red-600/30">MATCHED</span>}
                    <span className="text-[10px] text-faint ml-auto">{a.severity} / {a.urgency}</span>
                  </div>
                  {a.headline && <p className="text-xs text-muted mt-1">{a.headline}</p>}
                </div>
              ))}
            </div>
          )}

          {/* Forecast */}
          <h4 className="text-xs font-medium text-muted uppercase tracking-wider mb-2">Hourly Forecast</h4>
          {report.forecast.length === 0 ? (
            <p className="text-sm text-muted">No forecast data available.</p>
          ) : (
            <div className="space-y-2">
              {report.forecast.map((p, i) => (
                <div key={i} className="rounded border border-edge bg-field p-3">
                  <div className="flex items-center gap-3">
                    <span className="text-sm font-medium w-28 shrink-0">{p.name}</span>
                    <span className="text-sm">{p.temperature}°F</span>
                    <span className="text-xs text-muted">{p.wind} {p.wind_direction}</span>
                    <span className="text-xs text-muted ml-auto">{p.short_forecast}</span>
                  </div>
                </div>
              ))}
            </div>
          )}
        </WideModal>
      )}

      {form && (
        <div className="space-y-6">
          {/* General settings */}
          <div className="rounded-lg bg-panel border border-edge p-4 space-y-4">
            <h3 className="text-xs font-medium text-muted uppercase tracking-wider">Monitoring</h3>

            <label className="flex items-center gap-2 text-sm">
              <input type="checkbox" checked={form.enabled}
                onChange={(e) => setForm({ ...form, enabled: e.target.checked })} />
              Enable weather monitoring
            </label>

            <div className="grid grid-cols-2 gap-4">
              <Field label="Latitude" value={String(form.latitude)}
                onChange={(v) => setForm({ ...form, latitude: parseFloat(v) || 0 })} />
              <Field label="Longitude" value={String(form.longitude)}
                onChange={(v) => setForm({ ...form, longitude: parseFloat(v) || 0 })} />
              <Field label="NWS Alert Zones" value={form.alert_zones}
                onChange={(v) => setForm({ ...form, alert_zones: v })} />
              <Field label="Wind Threshold (mph)" value={String(form.wind_speed_mph)} type="number"
                onChange={(v) => setForm({ ...form, wind_speed_mph: parseInt(v) || 40 })} />
              <div>
                <Field label="Severe Keywords" value={form.severe_keywords}
                  onChange={(v) => setForm({ ...form, severe_keywords: v })} />
                <p className="text-[10px] text-faint mt-1">
                  Comma-separated words matched against NWS hourly forecast text (e.g. tornado, hail, damaging wind).
                </p>
              </div>
              <Field label="Poll Interval (seconds)" value={String(form.poll_interval)} type="number"
                onChange={(v) => setForm({ ...form, poll_interval: parseInt(v) || 300 })} />
            </div>

            <div>
              <label className="text-xs text-muted">Alert Types</label>
              <textarea value={form.alert_types}
                onChange={(e) => setForm({ ...form, alert_types: e.target.value })}
                className="block w-full bg-field border border-edge-strong rounded px-3 py-2 text-sm h-24" />
              <p className="text-[10px] text-faint mt-1">
                NWS alert event names that trigger severe weather mode (e.g. Tornado Warning, Severe Thunderstorm Warning).
                Matched as substrings against active alerts for your configured zones. Use the Weather Report to see current alert names.
              </p>
            </div>
          </div>

          {/* Parameter control */}
          <div className="rounded-lg bg-panel border border-edge p-4 space-y-4">
            <h3 className="text-xs font-medium text-muted uppercase tracking-wider">Parameter Override</h3>
            <p className="text-xs text-muted">
              During severe weather, the selected register is overridden with the severe value.
              When conditions clear, choose Restore to return to the pre-override value, or pick a fixed value.
            </p>

            {!upsRegs ? (
              <div className="grid grid-cols-3 gap-4">
                {[1,2,3].map(i => (
                  <div key={i}>
                    <div className="h-3 w-24 bg-field rounded animate-pulse mb-2" />
                    <div className="h-8 w-full bg-field rounded animate-pulse" />
                  </div>
                ))}
              </div>
            ) : (
              <div className="grid grid-cols-3 gap-4">
                <div>
                  <label className="text-xs text-muted">Control Register</label>
                  <select value={form.control_register}
                    onChange={(e) => setForm({ ...form, control_register: e.target.value, severe_raw_value: undefined, normal_raw_value: undefined })}
                    className="block w-full bg-field border border-edge-strong rounded px-3 py-1.5 text-sm">
                    {writableRegs.length > 0 ? (
                      writableRegs.map(r => (
                        <option key={r.name} value={r.name}>{r.display_name}</option>
                      ))
                    ) : (
                      <option value={form.control_register}>{form.control_register}</option>
                    )}
                  </select>
                </div>

                {selectedReg?.type === 'bitfield' && selectedReg.options ? (
                  <>
                    <div>
                      <label className="text-xs text-muted">Severe Value</label>
                      <select value={form.severe_raw_value ?? ''}
                        onChange={(e) => setForm({ ...form, severe_raw_value: parseInt(e.target.value) })}
                        className="block w-full bg-field border border-edge-strong rounded px-3 py-1.5 text-sm">
                        <option value="">Select...</option>
                        {selectedReg.options.map(o => (
                          <option key={o.value} value={o.value}>{o.label}</option>
                        ))}
                      </select>
                    </div>
                    <div>
                      <label className="text-xs text-muted">Clear Action</label>
                      <select value={form.normal_raw_value === undefined || form.normal_raw_value === -1 ? '-1' : String(form.normal_raw_value)}
                        onChange={(e) => setForm({ ...form, normal_raw_value: parseInt(e.target.value) })}
                        className="block w-full bg-field border border-edge-strong rounded px-3 py-1.5 text-sm">
                        <option value="-1">Restore (previous value)</option>
                        {selectedReg.options.map(o => (
                          <option key={o.value} value={o.value}>{o.label}</option>
                        ))}
                      </select>
                    </div>
                  </>
                ) : (
                  <>
                    <Field label="Severe Value (raw)" value={String(form.severe_raw_value ?? '')} type="number"
                      onChange={(v) => setForm({ ...form, severe_raw_value: v ? parseInt(v) : undefined })} />
                    <div>
                      <label className="text-xs text-muted">Clear Action</label>
                      <select value={form.normal_raw_value === undefined || form.normal_raw_value === -1 ? '-1' : 'fixed'}
                        onChange={(e) => setForm({ ...form, normal_raw_value: e.target.value === '-1' ? -1 : form.normal_raw_value === -1 ? 0 : form.normal_raw_value })}
                        className="block w-full bg-field border border-edge-strong rounded px-3 py-1.5 text-sm">
                        <option value="-1">Restore (previous value)</option>
                        <option value="fixed">Fixed value</option>
                      </select>
                      {form.normal_raw_value !== undefined && form.normal_raw_value !== -1 && (
                        <input type="number" value={form.normal_raw_value}
                          onChange={(e) => setForm({ ...form, normal_raw_value: parseInt(e.target.value) || 0 })}
                          className="block w-full bg-field border border-edge-strong rounded px-3 py-1.5 text-sm mt-1" />
                      )}
                    </div>
                  </>
                )}
              </div>
            )}
          </div>

          <div className="flex items-center gap-3">
            <button onClick={save} disabled={saving}
              className="px-4 py-2 bg-accent hover:bg-accent-hover text-white rounded text-sm disabled:opacity-50">
              {saving ? 'Saving...' : 'Save'}
            </button>
            {restarting && <span className="text-xs text-muted">Saved — restarting...</span>}
            {saved && !restarting && <span className="text-xs text-green-400">Saved</span>}
          </div>
        </div>
      )}
    </div>
  )
}

