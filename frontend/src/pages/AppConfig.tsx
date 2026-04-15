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

  // Group by prefix
  const groups = [...new Set(config.map((c) => c.key.split('.')[0]))]

  return (
    <div>
      <h2 className="text-xl font-semibold mb-4">App Settings</h2>
      <p className="text-sm text-gray-500 mb-4">
        Changes require daemon restart to take effect.
      </p>

      {groups.map((group) => (
        <div key={group} className="mb-6">
          <h3 className="text-sm font-medium text-gray-400 uppercase tracking-wider mb-2">
            {group}
          </h3>
          <div className="rounded-lg bg-gray-900 border border-gray-800 p-4 space-y-3">
            {config
              .filter((c) => c.key.startsWith(group + '.'))
              .map((c) => (
                <ConfigRow key={c.key} entry={c} saving={saving} onSave={handleSave} />
              ))}
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

  return (
    <div className="flex items-center gap-4">
      <div className="flex-1">
        <label className="text-sm text-gray-200">{subKey}</label>
        <p className="text-xs text-gray-500">{entry.description}</p>
      </div>
      <input
        type={entry.type === 'int' ? 'number' : 'text'}
        value={val}
        onChange={(e) => setVal(e.target.value)}
        className="bg-gray-800 border border-gray-700 rounded px-3 py-1.5 text-sm w-32"
      />
      {changed && (
        <button
          onClick={() => onSave(entry.key, val)}
          disabled={saving === entry.key}
          className="px-3 py-1.5 bg-blue-700 hover:bg-blue-600 rounded text-xs">
          {saving === entry.key ? '...' : 'Save'}
        </button>
      )}
    </div>
  )
}
