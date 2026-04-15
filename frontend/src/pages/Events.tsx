import { useApi } from '../hooks/useApi'

interface Event {
  timestamp: string
  severity: string
  category: string
  title: string
  message: string
}

const severityColor: Record<string, string> = {
  info: 'text-blue-400 bg-blue-900/30',
  warning: 'text-yellow-400 bg-yellow-900/30',
  error: 'text-red-400 bg-red-900/30',
  critical: 'text-red-300 bg-red-800/40',
}

const categoryColor: Record<string, string> = {
  status: 'bg-gray-700',
  alert: 'bg-orange-800',
  command: 'bg-indigo-800',
  shutdown: 'bg-red-800',
  weather: 'bg-sky-800',
  system: 'bg-gray-700',
}

export default function Events() {
  const { data: events, error, loading } = useApi<Event[]>('/api/events', 5000)

  return (
    <div>
      <h2 className="text-xl font-semibold mb-4">Events</h2>

      {error && <p className="text-red-400 mb-4">{error}</p>}
      {loading && <p className="text-gray-500">Loading...</p>}

      {events && events.length === 0 && (
        <p className="text-gray-500">No events yet.</p>
      )}

      {events && events.length > 0 && (
        <div className="rounded-lg border border-gray-800 overflow-hidden">
          <table className="w-full text-sm">
            <thead className="bg-gray-900 text-gray-400">
              <tr>
                <th className="text-left px-4 py-2">Time</th>
                <th className="text-left px-4 py-2">Severity</th>
                <th className="text-left px-4 py-2">Category</th>
                <th className="text-left px-4 py-2">Title</th>
                <th className="text-left px-4 py-2">Message</th>
              </tr>
            </thead>
            <tbody>
              {events.map((ev, i) => (
                <tr key={i} className="border-t border-gray-800 hover:bg-gray-900/50">
                  <td className="px-4 py-2 text-gray-400 whitespace-nowrap font-mono text-xs">
                    {ev.timestamp}
                  </td>
                  <td className="px-4 py-2">
                    <span className={`px-2 py-0.5 rounded text-xs ${severityColor[ev.severity] || 'text-gray-400'}`}>
                      {ev.severity}
                    </span>
                  </td>
                  <td className="px-4 py-2">
                    <span className={`px-2 py-0.5 rounded text-xs text-gray-300 ${categoryColor[ev.category] || 'bg-gray-700'}`}>
                      {ev.category}
                    </span>
                  </td>
                  <td className="px-4 py-2 text-gray-200">{ev.title}</td>
                  <td className="px-4 py-2 text-gray-400 truncate max-w-md">{ev.message}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
    </div>
  )
}
