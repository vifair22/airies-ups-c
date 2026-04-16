import { useState, useEffect } from 'react'
import { useApi, apiPost } from '../hooks/useApi'

interface WeatherStatus {
  enabled: boolean
  severe?: boolean
  reasons?: string
  latitude?: number
  longitude?: number
  alert_zones?: string
  wind_threshold_mph?: number
  poll_interval?: number
  control_register?: string
}

interface WeatherConfigData {
  enabled: boolean
  latitude: number
  longitude: number
  alert_zones: string
  alert_types: string
  wind_speed_mph: number
  severe_keywords: string
  poll_interval: number
  control_register: string
  severe_raw_value?: number
  normal_raw_value?: number
}

interface ConfigReg {
  name: string
  display_name: string
  writable: boolean
  type: string
  options?: { value: number; name: string; label: string }[]
}

export default function WeatherConfig() {
  const { data: status } = useApi<WeatherStatus>('/api/weather/status', 5000)
  const { data: config, refetch } = useApi<WeatherConfigData>('/api/weather/config')
  const { data: upsRegs } = useApi<ConfigReg[]>('/api/config/ups')
  const [form, setForm] = useState<WeatherConfigData | null>(null)
  const [saving, setSaving] = useState(false)
  const [saved, setSaved] = useState(false)

  useEffect(() => {
    if (config && !form) setForm(config)
  }, [config, form])

  const save = async () => {
    if (!form) return
    setSaving(true)
    try {
      await apiPost('/api/weather/config', form)
      await refetch()
      setSaved(true)
      setTimeout(() => setSaved(false), 3000)
    } catch {
      alert('Failed to save')
    }
    setSaving(false)
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
            ? 'bg-red-900/30 border-red-700'
            : status.enabled
              ? 'bg-green-900/30 border-green-700'
              : 'bg-gray-900 border-gray-800'
        }`}>
          <div className="flex items-center gap-3">
            <span className={`w-3 h-3 rounded-full ${
              status.severe ? 'bg-red-400' : status.enabled ? 'bg-green-400' : 'bg-gray-600'
            }`} />
            <span className="text-sm font-medium">
              {status.severe ? 'Severe Weather Active' : status.enabled ? 'Monitoring — All Clear' : 'Disabled'}
            </span>
          </div>
          {status.severe && status.reasons && (
            <p className="text-sm text-red-300 mt-2">{status.reasons}</p>
          )}
        </div>
      )}

      {form && (
        <div className="space-y-6">
          {/* General settings */}
          <div className="rounded-lg bg-gray-900 border border-gray-800 p-4 space-y-4">
            <h3 className="text-xs font-medium text-gray-400 uppercase tracking-wider">Monitoring</h3>

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
              <Field label="Severe Keywords" value={form.severe_keywords}
                onChange={(v) => setForm({ ...form, severe_keywords: v })} />
              <Field label="Poll Interval (seconds)" value={String(form.poll_interval)} type="number"
                onChange={(v) => setForm({ ...form, poll_interval: parseInt(v) || 300 })} />
            </div>

            <div>
              <label className="text-xs text-gray-400">Alert Types</label>
              <textarea value={form.alert_types}
                onChange={(e) => setForm({ ...form, alert_types: e.target.value })}
                className="block w-full bg-gray-800 border border-gray-700 rounded px-3 py-2 text-sm h-24" />
            </div>
          </div>

          {/* Parameter control */}
          <div className="rounded-lg bg-gray-900 border border-gray-800 p-4 space-y-4">
            <h3 className="text-xs font-medium text-gray-400 uppercase tracking-wider">Parameter Override</h3>
            <p className="text-xs text-gray-500">
              During severe weather, the selected register is overridden with the severe value.
              When conditions clear, the register is restored to the value it had before the override.
            </p>

            <div className="grid grid-cols-3 gap-4">
              <div>
                <label className="text-xs text-gray-400">Control Register</label>
                <select value={form.control_register}
                  onChange={(e) => setForm({ ...form, control_register: e.target.value, severe_raw_value: undefined, normal_raw_value: undefined })}
                  className="block w-full bg-gray-800 border border-gray-700 rounded px-3 py-1.5 text-sm">
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
                    <label className="text-xs text-gray-400">Severe Value</label>
                    <select value={form.severe_raw_value ?? ''}
                      onChange={(e) => setForm({ ...form, severe_raw_value: parseInt(e.target.value) })}
                      className="block w-full bg-gray-800 border border-gray-700 rounded px-3 py-1.5 text-sm">
                      <option value="">Select...</option>
                      {selectedReg.options.map(o => (
                        <option key={o.value} value={o.value}>{o.label}</option>
                      ))}
                    </select>
                  </div>
                  <div>
                    <label className="text-xs text-gray-400">Normal Value (fallback)</label>
                    <select value={form.normal_raw_value ?? ''}
                      onChange={(e) => setForm({ ...form, normal_raw_value: parseInt(e.target.value) })}
                      className="block w-full bg-gray-800 border border-gray-700 rounded px-3 py-1.5 text-sm">
                      <option value="">Select...</option>
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
                  <Field label="Normal Value (raw, fallback)" value={String(form.normal_raw_value ?? '')} type="number"
                    onChange={(v) => setForm({ ...form, normal_raw_value: v ? parseInt(v) : undefined })} />
                </>
              )}
            </div>
          </div>

          <div className="flex items-center gap-3">
            <button onClick={save} disabled={saving}
              className="px-4 py-2 bg-blue-700 hover:bg-blue-600 rounded text-sm disabled:opacity-50">
              {saving ? 'Saving...' : 'Save'}
            </button>
            {saved && <span className="text-xs text-green-400">Saved — restart to apply</span>}
          </div>
        </div>
      )}
    </div>
  )
}

function Field({ label, value, onChange, type = 'text' }: {
  label: string; value: string; onChange: (v: string) => void; type?: string
}) {
  return (
    <div>
      <label className="text-xs text-gray-400">{label}</label>
      <input type={type} value={value} onChange={(e) => onChange(e.target.value)}
        className="block w-full bg-gray-800 border border-gray-700 rounded px-3 py-1.5 text-sm" />
    </div>
  )
}
