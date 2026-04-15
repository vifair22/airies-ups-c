import { useState } from 'react'
import { useApi, apiPost } from '../hooks/useApi'

interface ConfigReg {
  name: string
  display_name: string
  unit?: string
  group?: string
  type: string
  raw_value: number
  value: number
  writable: boolean
  setting?: string
  setting_label?: string
  options?: { value: number; name: string; label: string }[]
}

export default function UpsConfig() {
  const { data: regs, error, loading, refetch } = useApi<ConfigReg[]>('/api/config/ups')
  const [saving, setSaving] = useState<string | null>(null)

  if (loading) return <p className="text-gray-500">Loading...</p>
  if (error) return <p className="text-red-400">{error}</p>
  if (!regs || regs.length === 0) return (
    <div>
      <h2 className="text-xl font-semibold mb-4">UPS Configuration Registers</h2>
      <p className="text-gray-500">No config registers available (UPS not connected or driver has none).</p>
    </div>
  )

  const groups = [...new Set(regs.map((r) => r.group || 'other'))]

  const handleWrite = async (name: string, value: number) => {
    setSaving(name)
    try {
      await apiPost('/api/config/ups', { name, value })
      await refetch()
    } catch {
      alert('Failed to write register')
    }
    setSaving(null)
  }

  return (
    <div>
      <h2 className="text-xl font-semibold mb-4">UPS Configuration Registers</h2>

      {groups.map((group) => (
        <div key={group} className="mb-6">
          <h3 className="text-sm font-medium text-gray-400 uppercase tracking-wider mb-2">
            {group}
          </h3>
          <div className="rounded-lg border border-gray-800 overflow-hidden">
            <table className="w-full text-sm">
              <thead className="bg-gray-900 text-gray-400">
                <tr>
                  <th className="text-left px-4 py-2">Register</th>
                  <th className="text-left px-4 py-2">Value</th>
                  <th className="text-left px-4 py-2">Raw</th>
                  <th className="text-left px-4 py-2">Action</th>
                </tr>
              </thead>
              <tbody>
                {regs
                  .filter((r) => (r.group || 'other') === group)
                  .map((reg) => (
                    <RegRow key={reg.name} reg={reg} saving={saving}
                      onWrite={handleWrite} />
                  ))}
              </tbody>
            </table>
          </div>
        </div>
      ))}
    </div>
  )
}

function RegRow({ reg, saving, onWrite }: {
  reg: ConfigReg
  saving: string | null
  onWrite: (name: string, value: number) => void
}) {
  const [editVal, setEditVal] = useState<string>('')
  const [editing, setEditing] = useState(false)

  const displayValue = reg.type === 'bitfield' && reg.setting_label
    ? reg.setting_label
    : `${reg.value}${reg.unit ? ` ${reg.unit}` : ''}`

  return (
    <tr className="border-t border-gray-800 hover:bg-gray-900/50">
      <td className="px-4 py-2">
        <p className="text-gray-200">{reg.display_name}</p>
        <p className="text-xs text-gray-500">{reg.name}</p>
      </td>
      <td className="px-4 py-2 text-gray-200">{displayValue}</td>
      <td className="px-4 py-2 text-gray-500 font-mono text-xs">{reg.raw_value}</td>
      <td className="px-4 py-2">
        {reg.writable && !editing && (
          <button onClick={() => { setEditing(true); setEditVal(String(reg.raw_value)) }}
            className="text-xs text-blue-400 hover:text-blue-300">
            Edit
          </button>
        )}
        {reg.writable && editing && (
          <div className="flex gap-2 items-center">
            {reg.type === 'bitfield' && reg.options ? (
              <select value={editVal} onChange={(e) => setEditVal(e.target.value)}
                className="bg-gray-800 border border-gray-700 rounded px-2 py-1 text-xs">
                {reg.options.map((o) => (
                  <option key={o.value} value={o.value}>{o.label}</option>
                ))}
              </select>
            ) : (
              <input type="number" value={editVal}
                onChange={(e) => setEditVal(e.target.value)}
                className="bg-gray-800 border border-gray-700 rounded px-2 py-1 text-xs w-20" />
            )}
            <button
              onClick={() => { onWrite(reg.name, parseInt(editVal)); setEditing(false) }}
              disabled={saving === reg.name}
              className="text-xs text-green-400 hover:text-green-300">
              {saving === reg.name ? '...' : 'Save'}
            </button>
            <button onClick={() => setEditing(false)}
              className="text-xs text-gray-500 hover:text-gray-400">
              Cancel
            </button>
          </div>
        )}
      </td>
    </tr>
  )
}
