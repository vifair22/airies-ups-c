import { useApi } from '../hooks/useApi'

interface Event {
  timestamp: string
  severity: string
  category: string
  title: string
  message: string
}

const severityStyle: Record<string, { dot: string; bg: string }> = {
  info:     { dot: 'bg-blue-400',   bg: 'bg-blue-900/30 text-blue-300' },
  warning:  { dot: 'bg-yellow-400', bg: 'bg-yellow-900/30 text-yellow-300' },
  error:    { dot: 'bg-red-400',    bg: 'bg-red-900/30 text-red-300' },
  critical: { dot: 'bg-red-500',    bg: 'bg-red-800/40 text-red-200' },
}

const categoryColor: Record<string, string> = {
  status: 'bg-gray-700', alert: 'bg-orange-800', command: 'bg-indigo-800',
  shutdown: 'bg-red-800', weather: 'bg-sky-800', system: 'bg-gray-700',
}

function relativeTime(ts: string): string {
  const diff = (Date.now() - new Date(ts + 'Z').getTime()) / 1000
  if (diff < 0) return 'just now'
  if (diff < 60) return `${Math.floor(diff)}s ago`
  if (diff < 3600) return `${Math.floor(diff / 60)}m ago`
  if (diff < 86400) return `${Math.floor(diff / 3600)}h ago`
  return `${Math.floor(diff / 86400)}d ago`
}

export default function Events() {
  const { data: events, error, loading } = useApi<Event[]>('/api/events', 5000)

  return (
    <div>
      <div className="flex items-center justify-between mb-4">
        <h2 className="text-xl font-semibold">Events</h2>
        {events && <span className="text-xs text-gray-500">{events.length} entries</span>}
      </div>

      {error && <p className="text-red-400 mb-4">{error}</p>}
      {loading && <LoadingSkeleton />}

      {events && events.length === 0 && (
        <div className="text-center py-12 text-gray-500">
          <p className="text-lg mb-1">No events yet</p>
          <p className="text-sm">Events will appear here as the UPS reports status changes.</p>
        </div>
      )}

      {events && events.length > 0 && (
        <div className="space-y-2">
          {events.map((ev, i) => {
            const sev = severityStyle[ev.severity] || severityStyle.info
            return (
              <div key={i} className="rounded-lg bg-gray-900 border border-gray-800 px-4 py-3">
                <div className="flex items-start gap-3">
                  <span className={`w-2 h-2 rounded-full mt-1.5 shrink-0 ${sev.dot}`} />
                  <div className="flex-1 min-w-0">
                    <div className="flex items-center gap-2 mb-0.5">
                      <span className="font-medium text-sm text-gray-200">{ev.title}</span>
                      <span className={`px-1.5 py-0.5 rounded text-[10px] ${categoryColor[ev.category] || 'bg-gray-700'} text-gray-300`}>
                        {ev.category}
                      </span>
                    </div>
                    <p className="text-sm text-gray-400 break-words">{ev.message}</p>
                  </div>
                  <span className="text-[11px] text-gray-600 whitespace-nowrap shrink-0" title={ev.timestamp}>
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
    <div className="space-y-2">
      {[1,2,3].map((i) => (
        <div key={i} className="rounded-lg bg-gray-900 border border-gray-800 h-16 animate-pulse" />
      ))}
    </div>
  )
}
