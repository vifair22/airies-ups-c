import { useState } from 'react'
import { useApi, apiPost } from '../hooks/useApi'

interface ConfigEntry {
  key: string
  value: string
  type: string
  default_value: string
  description: string
}

export default function AppConfig() {
  const { data: config, error, loading, refetch } = useApi<ConfigEntry[]>('/api/config/app')
  const [saving, setSaving] = useState<string | null>(null)

  if (loading) return <p className="text-gray-500">Loading...</p>
  if (error) return <p className="text-red-400">{error}</p>
  if (!config) return null

  const handleSave = async (key: string, value: string) => {
    setSaving(key)
    try {
      await apiPost('/api/config/app', { key, value })
      await refetch()
    } catch {
      alert('Failed to save')
    }
    setSaving(null)
  }

  const groups = [...new Set(config.map((c) => c.key.split('.')[0]))]

  return (
    <div>
      <h2 className="text-xl font-semibold mb-1">App Settings</h2>
      <p className="text-sm text-gray-500 mb-4">
        Changes to these settings require a daemon restart.
      </p>

      {groups.map((group) => (
        <div key={group} className="mb-4">
          <div className="rounded-lg bg-gray-900 border border-gray-800">
            <div className="px-4 py-2.5 border-b border-gray-800">
              <h3 className="text-xs font-medium text-gray-400 uppercase tracking-wider">{group}</h3>
            </div>
            <div className="divide-y divide-gray-800">
              {config
                .filter((c) => c.key.startsWith(group + '.'))
                .map((c) => (
                  <ConfigRow key={c.key} entry={c} saving={saving} onSave={handleSave} />
                ))}
            </div>
          </div>
        </div>
      ))}
    </div>
  )
}

function ConfigRow({ entry, saving, onSave }: {
  entry: ConfigEntry
  saving: string | null
  onSave: (key: string, value: string) => void
}) {
  const [val, setVal] = useState(entry.value)
  const changed = val !== entry.value
  const subKey = entry.key.split('.').slice(1).join('.')
  const isDefault = entry.value === entry.default_value

  return (
    <div className="flex items-center gap-4 px-4 py-3">
      <div className="flex-1 min-w-0">
        <div className="flex items-center gap-2">
          <label className="text-sm text-gray-200 font-medium">{subKey}</label>
          {!isDefault && (
            <span className="text-[10px] text-yellow-600">modified</span>
          )}
        </div>
        <p className="text-xs text-gray-500">{entry.description}</p>
        {entry.default_value && (
          <p className="text-[10px] text-gray-600 mt-0.5">default: {entry.default_value}</p>
        )}
      </div>
      <input
        type={entry.type === 'int' ? 'number' : 'text'}
        value={val}
        onChange={(e) => setVal(e.target.value)}
        className="bg-gray-800 border border-gray-700 rounded px-3 py-1.5 text-sm w-28 text-right font-mono"
      />
      {changed && (
        <button
          onClick={() => onSave(entry.key, val)}
          disabled={saving === entry.key}
          className="px-3 py-1.5 bg-blue-700 hover:bg-blue-600 rounded text-xs shrink-0 transition-colors">
          {saving === entry.key ? '...' : 'Save'}
        </button>
      )}
    </div>
  )
}
