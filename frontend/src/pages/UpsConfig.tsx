import { useState } from 'react'
import { useApi } from '../hooks/useApi'
import type { ConfigReg, ConfigWriteError } from '../types/config'

export default function UpsConfig() {
  const { data: regs, error, loading, refetch } = useApi<ConfigReg[]>('/api/config/ups')
  const [saving, setSaving] = useState<string | null>(null)
  const [writeResult, setWriteResult] = useState<{ name: string; result: string; detail?: string } | null>(null)

  if (loading) return (
    <div>
      <h2 className="text-xl font-semibold mb-4">UPS Configuration Registers</h2>
      <div className="rounded-lg border border-edge overflow-hidden">
        {/* Header */}
        <div className="bg-panel px-4 py-2.5 flex gap-8">
          <div className="flex-[4] h-3 w-20 bg-field rounded animate-pulse" />
          <div className="flex-[3] h-3 w-12 bg-field rounded animate-pulse" />
          <div className="flex-[1] h-3 w-8 bg-field rounded animate-pulse ml-auto" />
          <div className="flex-[2] h-3 w-12 bg-field rounded animate-pulse ml-auto" />
        </div>
        {/* Simulate 6 groups with varying register counts */}
        {[3, 1, 9, 6, 2, 2].map((count, gi) => (
          <div key={gi}>
            {/* Group header */}
            <div className="bg-panel/50 border-t border-edge px-4 py-1.5">
              <div className="h-2.5 w-28 bg-field/50 rounded animate-pulse" />
            </div>
            {/* Register rows */}
            {Array.from({ length: count }).map((_, ri) => (
              <div key={ri} className="border-t border-edge px-4 py-3 flex gap-8">
                <div className="flex-[4]">
                  <div className="h-4 w-48 bg-field rounded animate-pulse mb-1" />
                  <div className="h-3 w-32 bg-field/50 rounded animate-pulse" />
                </div>
                <div className="flex-[3]">
                  <div className="h-4 w-24 bg-field rounded animate-pulse" />
                </div>
                <div className="flex-[1]">
                  <div className="h-4 w-8 bg-field rounded animate-pulse ml-auto" />
                </div>
                <div className="flex-[2]">
                  <div className="h-4 w-12 bg-field rounded animate-pulse ml-auto" />
                </div>
              </div>
            ))}
          </div>
        ))}
      </div>
    </div>
  )

  if (error) return <p className="text-red-400">{error}</p>
  if (!regs || regs.length === 0) return (
    <div>
      <h2 className="text-xl font-semibold mb-4">UPS Configuration Registers</h2>
      <div className="rounded-lg bg-panel border border-edge text-center py-12">
        <p className="text-muted">UPS not connected. Registers unavailable.</p>
      </div>
    </div>
  )

  const groups = [...new Set(regs.map((r) => r.group || 'other'))]

  const handleWrite = async (name: string, value: number) => {
    setSaving(name)
    try {
      /* Use fetch directly so we can distinguish 400 (operator error,
       * structured ConfigWriteError body) from 200 (write attempt,
       * with result=written|rejected) and from 5xx (UPS/network). */
      const token = localStorage.getItem('auth_token')
      const res = await fetch('/api/config/ups', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
          ...(token ? { Authorization: `Bearer ${token}` } : {}),
        },
        body: JSON.stringify({ name, value }),
      })
      const body = await res.json()
      if (res.status === 400 && body && body.error === 'out_of_range') {
        const e = body as ConfigWriteError
        let detail = `value ${e.attempted_value} is out of range`
        if (e.min !== undefined && e.max !== undefined)
          detail = `value ${e.attempted_value} outside ${e.min}–${e.max}`
        else if (e.accepted_values)
          detail = `value ${e.attempted_value} is not one of ${e.accepted_values.join(', ')}`
        setWriteResult({ name, result: 'invalid', detail })
      } else if (res.ok && body && body.result) {
        setWriteResult({ name, result: body.result })
        await refetch()
      } else {
        setWriteResult({ name, result: 'error' })
      }
      setTimeout(() => setWriteResult(null), 5000)
    } catch {
      setWriteResult({ name, result: 'error' })
      setTimeout(() => setWriteResult(null), 5000)
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

      <div className="rounded-lg border border-edge overflow-hidden">
        <table className="w-full text-sm table-fixed">
          <colgroup>
            <col className="w-[40%]" />
            <col className="w-[30%]" />
            <col className="w-[10%]" />
            <col className="w-[20%]" />
          </colgroup>
          <thead className="bg-panel text-muted text-xs uppercase tracking-wider">
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
                <tr key={`hdr-${group}`} className="bg-panel/50">
                  <td colSpan={4} className="px-4 py-1.5 text-[10px] text-muted uppercase tracking-widest font-medium">
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
                      feedbackDetail={writeResult?.name === reg.name ? writeResult.detail : undefined}
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

function RegRow({ reg, displayValue, saving, onWrite, feedback, feedbackDetail }: {
  reg: ConfigReg
  displayValue: string
  saving: string | null
  onWrite: (name: string, value: number) => void
  feedback: string | null
  feedbackDetail?: string
}) {
  const [editVal, setEditVal] = useState<string>('')
  const [editing, setEditing] = useState(false)

  const rowBg = feedback === 'written'
    ? 'bg-green-900/20 border-t border-edge'
    : feedback === 'rejected'
      ? 'bg-yellow-900/20 border-t border-edge'
      : feedback === 'invalid'
        ? 'bg-amber-900/20 border-t border-edge'
        : feedback === 'error'
          ? 'bg-red-900/20 border-t border-edge'
          : saving === reg.name
            ? 'bg-blue-900/10 border-t border-edge'
            : 'border-t border-edge hover:bg-panel/30'

  return (
    <tr className={`transition-colors duration-300 ${rowBg}`}>
      <td className="px-4 py-2">
        <p className="text-primary text-sm">{reg.display_name}</p>
        <p className="text-[11px] text-faint font-mono">{reg.name}</p>
      </td>
      <td className="px-4 py-2 text-primary text-sm">
        {saving === reg.name ? (
          <span className="text-muted animate-pulse">saving...</span>
        ) : (
          displayValue
        )}
      </td>
      <td className="px-4 py-2 text-right text-faint font-mono text-xs">{reg.raw_value}</td>
      <td className="px-4 py-2 text-right">
        {feedback && (
          <span
            title={feedbackDetail}
            className={`text-xs font-medium ${
              feedback === 'written' ? 'text-green-400'
              : feedback === 'rejected' ? 'text-yellow-400'
              : feedback === 'invalid' ? 'text-amber-400'
              : 'text-red-400'
            }`}
          >
            {feedback === 'written' ? 'saved'
              : feedback === 'rejected' ? 'rejected by UPS'
              : feedback === 'invalid' ? (feedbackDetail || 'invalid value')
              : 'error'}
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
          <span className="text-[10px] text-faint">read-only</span>
        )}
        {reg.writable && editing && (
          <div className="flex gap-1.5 items-center justify-end">
            {reg.type === 'bitfield' && reg.options ? (
              <select
                value={editVal}
                onChange={(e) => setEditVal(e.target.value)}
                className="bg-field border border-edge-strong rounded px-2 py-1 text-xs"
              >
                {reg.options.map((o) => (
                  <option key={o.value} value={o.value}>{o.label}</option>
                ))}
              </select>
            ) : (
              <>
                <input
                  type="number"
                  value={editVal}
                  onChange={(e) => setEditVal(e.target.value)}
                  min={reg.min}
                  max={reg.max}
                  step={reg.step ?? 1}
                  className="bg-field border border-edge-strong rounded px-2 py-1 text-xs w-20 text-right"
                />
                {/* Range hint — visible while editing so the operator
                    sees the constraint before clicking Save. The same
                    bounds are enforced server-side via the descriptor's
                    meta.scalar.{min,max} (see ups_config_validate). */}
                {reg.min !== undefined && reg.max !== undefined && (
                  <span className="text-[10px] text-faint whitespace-nowrap">
                    {reg.min}–{reg.max}{reg.unit ? ` ${reg.unit}` : ''}
                  </span>
                )}
              </>
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
              className="text-xs text-muted hover:text-muted"
            >
              Cancel
            </button>
          </div>
        )}
      </td>
    </tr>
  )
}
