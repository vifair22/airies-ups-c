import { NavLink, Outlet } from 'react-router-dom'
import { useApi } from '../hooks/useApi'

interface StatusBrief {
  driver: string
  connected: boolean
}

const nav = [
  { to: '/', label: 'Dashboard', icon: '~' },
  { to: '/events', label: 'Events', icon: '!' },
  { to: '/telemetry', label: 'Telemetry', icon: '#' },
  { to: '/commands', label: 'Commands', icon: '>' },
]

const configNav = [
  { to: '/config/ups', label: 'UPS Registers' },
  { to: '/config/app', label: 'App Settings' },
  { to: '/config/shutdown', label: 'Shutdown' },
  { to: '/config/weather', label: 'Weather' },
]

function SideLink({ to, label }: { to: string; label: string }) {
  return (
    <NavLink
      to={to}
      end={to === '/'}
      className={({ isActive }) =>
        `block px-3 py-1.5 rounded text-sm transition-colors ${
          isActive
            ? 'bg-gray-800 text-white font-medium'
            : 'text-gray-400 hover:text-gray-200 hover:bg-gray-800/50'
        }`
      }
    >
      {label}
    </NavLink>
  )
}

export default function Layout() {
  const { data: status } = useApi<StatusBrief>('/api/status', 5000)

  return (
    <div className="flex min-h-screen bg-gray-950 text-gray-100">
      <aside className="w-52 border-r border-gray-800 px-3 py-4 flex flex-col gap-0.5 shrink-0">
        <div className="px-3 py-2 mb-3">
          <h1 className="text-lg font-semibold tracking-tight">airies-ups</h1>
          <div className="flex items-center gap-1.5 mt-1">
            <span className={`w-2 h-2 rounded-full ${
              status?.connected ? 'bg-green-400' : 'bg-gray-600'
            }`} />
            <span className="text-xs text-gray-500">
              {status?.connected ? `${status.driver} connected` : 'disconnected'}
            </span>
          </div>
        </div>

        {nav.map((n) => (
          <SideLink key={n.to} to={n.to} label={n.label} />
        ))}

        <p className="text-[10px] text-gray-600 uppercase tracking-widest px-3 mt-5 mb-1">
          Configuration
        </p>
        {configNav.map((n) => (
          <SideLink key={n.to} {...n} />
        ))}

        <div className="mt-auto px-3 py-2 text-[10px] text-gray-700 font-mono">
          v0.1.0
        </div>
      </aside>

      <main className="flex-1 p-6 overflow-auto min-w-0">
        <Outlet />
      </main>
    </div>
  )
}
