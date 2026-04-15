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
  date?: string
  options?: { value: number; name: string; label: string }[]
}

export default function UpsConfig() {
  const { data: regs, error, loading, refetch } = useApi<ConfigReg[]>('/api/config/ups')
  const [saving, setSaving] = useState<string | null>(null)
  const [writeResult, setWriteResult] = useState<{ name: string; result: string } | null>(null)

  if (loading) return (
    <div>
      <h2 className="text-xl font-semibold mb-4">UPS Configuration Registers</h2>
      <div className="rounded-lg border border-gray-800 overflow-hidden">
        <div className="bg-gray-900 px-4 py-2.5">
          <div className="h-3 w-24 bg-gray-800 rounded animate-pulse" />
        </div>
        {[1,2,3,4,5,6,7,8].map(i => (
          <div key={i} className="border-t border-gray-800 px-4 py-3 flex gap-8">
            <div className="flex-[4]">
              <div className="h-4 w-48 bg-gray-800 rounded animate-pulse mb-1" />
              <div className="h-3 w-32 bg-gray-800/50 rounded animate-pulse" />
            </div>
            <div className="flex-[3]">
              <div className="h-4 w-24 bg-gray-800 rounded animate-pulse" />
            </div>
            <div className="flex-[1]">
              <div className="h-4 w-8 bg-gray-800 rounded animate-pulse ml-auto" />
            </div>
            <div className="flex-[2]">
              <div className="h-4 w-12 bg-gray-800 rounded animate-pulse ml-auto" />
            </div>
          </div>
        ))}
      </div>
    </div>
  )

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
      const res = await apiPost<{ result: string }>('/api/config/ups', { name, value })
      setWriteResult({ name, result: res.result })
      await refetch()
      setTimeout(() => setWriteResult(null), 4000)
    } catch {
      setWriteResult({ name, result: 'error' })
      setTimeout(() => setWriteResult(null), 4000)
    }
    setSaving(null)
  }

  const formatValue = (reg: ConfigReg): string => {
    if (reg.date) return reg.date
    if (reg.type === 'bitfield' && reg.setting_label) return reg.setting_label
    if (reg.unit) return `${reg.value} ${reg.unit}`
    return String(reg.value)
  }

  return (
    <div>
      <h2 className="text-xl font-semibold mb-4">UPS Configuration Registers</h2>

      <div className="rounded-lg border border-gray-800 overflow-hidden">
        <table className="w-full text-sm table-fixed">
          <colgroup>
            <col className="w-[40%]" />
            <col className="w-[30%]" />
            <col className="w-[10%]" />
            <col className="w-[20%]" />
          </colgroup>
          <thead className="bg-gray-900 text-gray-400 text-xs uppercase tracking-wider">
            <tr>
              <th className="text-left px-4 py-2.5">Register</th>
              <th className="text-left px-4 py-2.5">Value</th>
              <th className="text-right px-4 py-2.5">Raw</th>
              <th className="text-right px-4 py-2.5">Action</th>
            </tr>
          </thead>
          <tbody>
            {groups.map((group) => (
              <>
                <tr key={`hdr-${group}`} className="bg-gray-900/50">
                  <td colSpan={4} className="px-4 py-1.5 text-[10px] text-gray-500 uppercase tracking-widest font-medium">
                    {group.replace(/_/g, ' ')}
                  </td>
                </tr>
                {regs
                  .filter((r) => (r.group || 'other') === group)
                  .map((reg) => (
                    <RegRow
                      key={reg.name}
                      reg={reg}
                      displayValue={formatValue(reg)}
                      saving={saving}
                      onWrite={handleWrite}
                      feedback={writeResult?.name === reg.name ? writeResult.result : null}
                    />
                  ))}
              </>
            ))}
          </tbody>
        </table>
      </div>
    </div>
  )
}

function RegRow({ reg, displayValue, saving, onWrite, feedback }: {
  reg: ConfigReg
  displayValue: string
  saving: string | null
  onWrite: (name: string, value: number) => void
  feedback: string | null
}) {
  const [editVal, setEditVal] = useState<string>('')
  const [editing, setEditing] = useState(false)

  const rowBg = feedback === 'written'
    ? 'bg-green-900/20 border-t border-gray-800'
    : feedback === 'rejected'
      ? 'bg-yellow-900/20 border-t border-gray-800'
      : feedback === 'error'
        ? 'bg-red-900/20 border-t border-gray-800'
        : saving === reg.name
          ? 'bg-blue-900/10 border-t border-gray-800'
          : 'border-t border-gray-800 hover:bg-gray-900/30'

  return (
    <tr className={`transition-colors duration-300 ${rowBg}`}>
      <td className="px-4 py-2">
        <p className="text-gray-200 text-sm">{reg.display_name}</p>
        <p className="text-[11px] text-gray-600 font-mono">{reg.name}</p>
      </td>
      <td className="px-4 py-2 text-gray-200 text-sm">
        {saving === reg.name ? (
          <span className="text-gray-500 animate-pulse">saving...</span>
        ) : (
          displayValue
        )}
      </td>
      <td className="px-4 py-2 text-right text-gray-600 font-mono text-xs">{reg.raw_value}</td>
      <td className="px-4 py-2 text-right">
        {feedback && (
          <span className={`text-xs font-medium ${
            feedback === 'written' ? 'text-green-400'
            : feedback === 'rejected' ? 'text-yellow-400'
            : 'text-red-400'
          }`}>
            {feedback === 'written' ? 'saved' : feedback === 'rejected' ? 'rejected by UPS' : 'error'}
          </span>
        )}
        {!feedback && reg.writable && !editing && (
          <button
            onClick={() => { setEditing(true); setEditVal(String(reg.raw_value)) }}
            className="text-xs text-blue-400 hover:text-blue-300"
          >
            Edit
          </button>
        )}
        {!feedback && !reg.writable && (
          <span className="text-[10px] text-gray-700">read-only</span>
        )}
        {reg.writable && editing && (
          <div className="flex gap-1.5 items-center justify-end">
            {reg.type === 'bitfield' && reg.options ? (
              <select
                value={editVal}
                onChange={(e) => setEditVal(e.target.value)}
                className="bg-gray-800 border border-gray-700 rounded px-2 py-1 text-xs"
              >
                {reg.options.map((o) => (
                  <option key={o.value} value={o.value}>{o.label}</option>
                ))}
              </select>
            ) : (
              <input
                type="number"
                value={editVal}
                onChange={(e) => setEditVal(e.target.value)}
                className="bg-gray-800 border border-gray-700 rounded px-2 py-1 text-xs w-20 text-right"
              />
            )}
            <button
              onClick={() => { onWrite(reg.name, parseInt(editVal)); setEditing(false) }}
              disabled={saving === reg.name}
              className="text-xs text-green-400 hover:text-green-300"
            >
              {saving === reg.name ? '...' : 'Save'}
            </button>
            <button
              onClick={() => setEditing(false)}
              className="text-xs text-gray-500 hover:text-gray-400"
            >
              Cancel
            </button>
          </div>
        )}
      </td>
    </tr>
  )
}
