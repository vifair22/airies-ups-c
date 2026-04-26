import { useApi } from '../hooks/useApi'
import type { ConfigReg, ConfigCategory } from '../types/config'

interface AboutResponse {
  inventory?: {
    model: string
    sku?: string
    serial: string
    firmware: string
    nominal_va: number
    nominal_watts: number
    sog_config: number
  }
  registers: ConfigReg[]
}

/* Display title and ordering for the four categories. Anything the
 * backend hasn't tagged falls back to "config" via the type default. */
const CATEGORY_ORDER: ConfigCategory[] = ['identity', 'measurement', 'config', 'diagnostic']
const CATEGORY_TITLE: Record<ConfigCategory, string> = {
  identity:    'Identity',
  measurement: 'Measurements',
  config:      'Configuration',
  diagnostic:  'Diagnostics',
}

function formatRegValue(reg: ConfigReg): string {
  if (reg.is_sentinel) return 'N/A'
  if (reg.type === 'string' && reg.string_value !== undefined) return reg.string_value
  if (reg.date) return reg.date
  if (reg.type === 'bitfield' && reg.setting_label) return reg.setting_label
  if (reg.type === 'flags') {
    if (!reg.active_flags || reg.active_flags.length === 0) return 'none'
    return reg.active_flags.map((f) => f.label).join(', ')
  }
  if (reg.unit) return `${reg.value} ${reg.unit}`
  return String(reg.value)
}

function formatRaw(reg: ConfigReg): string {
  /* String registers don't have a meaningful raw single-register value
   * (only the first register's bytes are reported). Hide the raw column
   * for them entirely. */
  if (reg.type === 'string') return ''
  /* Flags always render as 8-char hex regardless of value — the descriptor
   * spans uint32 conceptually even when the active bits all fit in low 16. */
  if (reg.type === 'flags')
    return `0x${reg.raw_value.toString(16).toUpperCase().padStart(8, '0')}`
  /* Other 32-bit composites use enough hex digits for their value. */
  if (reg.raw_value > 0xFFFF)
    return `0x${reg.raw_value.toString(16).toUpperCase().padStart(8, '0')}`
  return String(reg.raw_value)
}

export default function About() {
  const { data, error, loading } = useApi<AboutResponse>('/api/about')

  if (loading) return (
    <div>
      <h2 className="text-xl font-semibold mb-4">About this UPS</h2>

      {/* Device skeleton — 6 inventory rows */}
      <section className="mb-6">
        <div className="h-2.5 w-12 bg-field/50 rounded animate-pulse mb-2 ml-1" />
        <div className="rounded-lg border border-edge overflow-hidden">
          {Array.from({ length: 6 }).map((_, i) => (
            <div key={i}
                 className={`flex items-center justify-between px-4 py-2.5 ${i > 0 ? 'border-t border-edge' : ''}`}>
              <div className="h-3 w-28 bg-field rounded animate-pulse" />
              <div className="h-3 w-40 bg-field rounded animate-pulse" />
            </div>
          ))}
        </div>
      </section>

      {/* Registers skeleton — one section per category, grouped table */}
      {[1, 2].map((sec) => (
        <section key={sec} className="mb-6">
          <div className="h-2.5 w-24 bg-field/50 rounded animate-pulse mb-2 ml-1" />
          <div className="rounded-lg border border-edge overflow-hidden">
            <div className="bg-panel px-4 py-2.5 flex gap-8">
              <div className="flex-[5] h-3 w-12 bg-field rounded animate-pulse" />
              <div className="flex-[3] h-3 w-12 bg-field rounded animate-pulse" />
              <div className="flex-[2] h-3 w-8 bg-field rounded animate-pulse ml-auto" />
            </div>
            {[2, 3].map((count, gi) => (
              <div key={gi}>
                <div className="bg-panel/50 border-t border-edge px-4 py-1.5">
                  <div className="h-2.5 w-20 bg-field/50 rounded animate-pulse" />
                </div>
                {Array.from({ length: count }).map((_, ri) => (
                  <div key={ri} className="border-t border-edge px-4 py-3 flex gap-8">
                    <div className="flex-[5]">
                      <div className="h-4 w-48 bg-field rounded animate-pulse mb-1" />
                      <div className="h-3 w-32 bg-field/50 rounded animate-pulse" />
                    </div>
                    <div className="flex-[3]">
                      <div className="h-4 w-24 bg-field rounded animate-pulse" />
                    </div>
                    <div className="flex-[2]">
                      <div className="h-4 w-12 bg-field rounded animate-pulse ml-auto" />
                    </div>
                  </div>
                ))}
              </div>
            ))}
          </div>
        </section>
      ))}
    </div>
  )
  if (error) return <p className="text-red-400">{error}</p>
  if (!data) return null

  const inv = data.inventory
  const regs = data.registers || []

  const inventoryEntries: { label: string; value: string }[] = []
  if (inv) {
    if (inv.model)         inventoryEntries.push({ label: 'Model',             value: inv.model })
    if (inv.sku)           inventoryEntries.push({ label: 'SKU',               value: inv.sku })
    if (inv.serial)        inventoryEntries.push({ label: 'Serial Number',     value: inv.serial })
    if (inv.firmware)      inventoryEntries.push({ label: 'Firmware',          value: inv.firmware })
    if (inv.nominal_va)    inventoryEntries.push({ label: 'Nominal VA',        value: String(inv.nominal_va) })
    if (inv.nominal_watts) inventoryEntries.push({ label: 'Nominal Watts',     value: String(inv.nominal_watts) })
    if (inv.sog_config)    inventoryEntries.push({ label: 'SOG Configuration', value: String(inv.sog_config) })
  }

  if (inventoryEntries.length === 0 && regs.length === 0) {
    return (
      <div>
        <h2 className="text-xl font-semibold mb-4">About this UPS</h2>
        <div className="rounded-lg bg-panel border border-edge text-center py-12">
          <p className="text-muted">No device information available. UPS not connected.</p>
        </div>
      </div>
    )
  }

  /* Bucket registers by category, then by group within each category. */
  const byCategory = new Map<ConfigCategory, ConfigReg[]>()
  for (const reg of regs) {
    const cat = reg.category ?? 'config'
    if (!byCategory.has(cat)) byCategory.set(cat, [])
    byCategory.get(cat)!.push(reg)
  }

  return (
    <div>
      <h2 className="text-xl font-semibold mb-4">About this UPS</h2>

      {inventoryEntries.length > 0 && (
        <section className="mb-6">
          <h3 className="text-[10px] uppercase tracking-widest text-muted mb-2 px-1">Device</h3>
          <div className="rounded-lg border border-edge overflow-hidden">
            {inventoryEntries.map((e, i) => (
              <div key={e.label}
                   className={`flex items-center justify-between px-4 py-2.5 ${i > 0 ? 'border-t border-edge' : ''}`}>
                <span className="text-sm text-muted">{e.label}</span>
                <span className="text-sm text-primary font-mono">{e.value}</span>
              </div>
            ))}
          </div>
        </section>
      )}

      {CATEGORY_ORDER.map((cat) => {
        const items = byCategory.get(cat)
        if (!items || items.length === 0) return null
        const groups = [...new Set(items.map((r) => r.group || 'other'))]
        return (
          <section key={cat} className="mb-6">
            <h3 className="text-[10px] uppercase tracking-widest text-muted mb-2 px-1">{CATEGORY_TITLE[cat]}</h3>
            <div className="rounded-lg border border-edge overflow-hidden">
              <table className="w-full text-sm table-fixed">
                <colgroup>
                  <col className="w-[50%]" />
                  <col className="w-[30%]" />
                  <col className="w-[20%]" />
                </colgroup>
                <thead className="bg-panel text-muted text-xs uppercase tracking-wider">
                  <tr>
                    <th className="text-left px-4 py-2.5">Field</th>
                    <th className="text-left px-4 py-2.5">Value</th>
                    <th className="text-right px-4 py-2.5">Raw</th>
                  </tr>
                </thead>
                <tbody>
                  {groups.map((group) => (
                    <RegGroupRows
                      key={`${cat}-${group}`}
                      group={group}
                      items={items.filter((r) => (r.group || 'other') === group)}
                    />
                  ))}
                </tbody>
              </table>
            </div>
          </section>
        )
      })}
    </div>
  )
}

function RegGroupRows({ group, items }: { group: string; items: ConfigReg[] }) {
  return (
    <>
      <tr className="bg-panel/50">
        <td colSpan={3} className="px-4 py-1.5 text-[10px] text-muted uppercase tracking-widest font-medium">
          {group.replace(/_/g, ' ')}
        </td>
      </tr>
      {items.map((reg) => (
        <tr key={reg.name} className={`border-t border-edge ${reg.is_sentinel ? 'opacity-60' : ''}`}>
          <td className="px-4 py-2">
            <p className="text-primary text-sm">{reg.display_name}</p>
            <p className="text-[11px] text-faint font-mono">{reg.name}</p>
          </td>
          <td className="px-4 py-2 text-primary text-sm">{formatRegValue(reg)}</td>
          <td className="px-4 py-2 text-right text-faint font-mono text-xs">{formatRaw(reg)}</td>
        </tr>
      ))}
    </>
  )
}
