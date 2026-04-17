import { useState, useMemo } from 'react'
import { useApi } from '../hooks/useApi'

interface Event {
  timestamp: string
  severity: string
  category: string
  title: string
  message: string
}

const severityStyle: Record<string, { dot: string; bg: string }> = {
  info:     { dot: 'bg-blue-400',   bg: 'bg-blue-500/15 text-blue-700' },
  warning:  { dot: 'bg-yellow-400', bg: 'bg-yellow-500/15 text-yellow-700' },
  error:    { dot: 'bg-red-400',    bg: 'bg-red-500/15 text-red-600' },
  critical: { dot: 'bg-red-500',    bg: 'bg-red-500/25 text-red-600' },
}

const categoryColor: Record<string, string> = {
  system:   'bg-field-hover',
  status:   'bg-field-hover',
  power:    'bg-amber-500/20',
  mode:     'bg-indigo-500/20',
  fault:    'bg-red-500/20',
  alert:    'bg-orange-500/20',
  test:     'bg-sky-500/20',
  command:  'bg-indigo-500/20',
  shutdown: 'bg-red-500/20',
  weather:  'bg-sky-500/20',
  auth:     'bg-violet-500/20',
}

function relativeTime(ts: string): string {
  const diff = (Date.now() - new Date(ts + 'Z').getTime()) / 1000
  if (diff < 0) return 'just now'
  if (diff < 60) return `${Math.floor(diff)}s ago`
  if (diff < 3600) return `${Math.floor(diff / 60)}m ago`
  if (diff < 86400) return `${Math.floor(diff / 3600)}h ago`
  return `${Math.floor(diff / 86400)}d ago`
}

function FilterChip({ label, active, color, onClick }: {
  label: string; active: boolean; color?: string; onClick: () => void
}) {
  return (
    <button onClick={onClick}
      className={`px-2.5 py-1 rounded-full text-[11px] font-medium border transition-colors ${
        active
          ? `${color || 'bg-field-hover'} border-edge-strong text-primary`
          : 'bg-transparent border-edge text-faint hover:text-muted hover:border-edge-strong'
      }`}>
      {label}
    </button>
  )
}

export default function Events() {
  const { data: events, error, loading } = useApi<Event[]>('/api/events', 5000)

  const [severityFilter, setSeverityFilter] = useState<Set<string>>(new Set())
  const [categoryFilter, setCategoryFilter] = useState<Set<string>>(new Set())

  /* Derive available categories from actual data */
  const availableCategories = useMemo(() => {
    if (!events) return []
    const cats = new Set(events.map(e => e.category))
    return [...cats].sort()
  }, [events])

  const toggleFilter = (set: Set<string>, value: string, setter: (s: Set<string>) => void) => {
    const next = new Set(set)
    if (next.has(value)) next.delete(value)
    else next.add(value)
    setter(next)
  }

  const filtered = useMemo(() => {
    if (!events) return []
    return events.filter(e => {
      if (severityFilter.size > 0 && !severityFilter.has(e.severity)) return false
      if (categoryFilter.size > 0 && !categoryFilter.has(e.category)) return false
      return true
    })
  }, [events, severityFilter, categoryFilter])

  const hasFilters = severityFilter.size > 0 || categoryFilter.size > 0

  return (
    <div>
      <div className="flex items-center justify-between mb-4">
        <h2 className="text-xl font-semibold">Events</h2>
        {events && (
          <span className="text-xs text-muted">
            {hasFilters ? `${filtered.length} of ${events.length}` : events.length} entries
          </span>
        )}
      </div>

      {/* Filters */}
      <div className="flex flex-wrap items-center gap-2 mb-4">
        <span className="text-[10px] text-faint uppercase tracking-wider mr-1">Severity</span>
        {['info', 'warning', 'error', 'critical'].map(s => (
          <FilterChip key={s} label={s}
            active={severityFilter.has(s)}
            color={severityStyle[s]?.bg}
            onClick={() => toggleFilter(severityFilter, s, setSeverityFilter)}
          />
        ))}

        <span className="w-px h-4 bg-edge mx-1" />

        <span className="text-[10px] text-faint uppercase tracking-wider mr-1">Category</span>
        {availableCategories.map(c => (
          <FilterChip key={c} label={c}
            active={categoryFilter.has(c)}
            color={categoryColor[c]}
            onClick={() => toggleFilter(categoryFilter, c, setCategoryFilter)}
          />
        ))}

        {hasFilters && (
          <>
            <span className="w-px h-4 bg-edge mx-1" />
            <button onClick={() => { setSeverityFilter(new Set()); setCategoryFilter(new Set()) }}
              className="text-[11px] text-muted hover:text-secondary">
              Clear filters
            </button>
          </>
        )}
      </div>

      {error && <p className="text-red-400 mb-4">{error}</p>}
      {loading && <LoadingSkeleton />}

      {events && filtered.length === 0 && (
        <div className="text-center py-12 text-muted">
          {hasFilters ? (
            <p className="text-sm">No events match the current filters.</p>
          ) : (
            <>
              <p className="text-lg mb-1">No events yet</p>
              <p className="text-sm">Events will appear here as the UPS reports status changes.</p>
            </>
          )}
        </div>
      )}

      {filtered.length > 0 && (
        <div className="space-y-1.5">
          {filtered.map((ev, i) => {
            const sev = severityStyle[ev.severity] || severityStyle.info
            return (
              <div key={i} className="rounded-lg bg-panel border border-edge px-4 py-3">
                <div className="flex items-start gap-3">
                  <span className={`w-2 h-2 rounded-full mt-1.5 shrink-0 ${sev.dot}`} />
                  <div className="flex-1 min-w-0">
                    <div className="flex items-center gap-2 mb-0.5">
                      <span className="font-medium text-sm text-primary">{ev.title}</span>
                      <span className={`px-1.5 py-0.5 rounded text-[10px] ${categoryColor[ev.category] || 'bg-field-hover'} text-secondary`}>
                        {ev.category}
                      </span>
                      <span className={`px-1.5 py-0.5 rounded text-[10px] ${sev.bg}`}>
                        {ev.severity}
                      </span>
                    </div>
                    <p className="text-sm text-muted break-words">{ev.message}</p>
                  </div>
                  <span className="text-[11px] text-faint whitespace-nowrap shrink-0" title={ev.timestamp + ' UTC'}>
                    {relativeTime(ev.timestamp)}
                  </span>
                </div>
              </div>
            )
          })}
        </div>
      )}
    </div>
  )
}

function LoadingSkeleton() {
  return (
    <div className="space-y-1.5">
      {[1,2,3,4,5,6].map((i) => (
        <div key={i} className="rounded-lg bg-panel border border-edge h-16 animate-pulse" />
      ))}
    </div>
  )
}
