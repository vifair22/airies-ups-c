import { useEffect } from 'react'
import { NavLink, Outlet } from 'react-router-dom'
import { useApi, apiPost } from '../hooks/useApi'
import type { UpsStatus } from '../types/ups'

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
            ? 'bg-field text-primary font-medium'
            : 'text-muted hover:text-primary hover:bg-field/50'
        }`
      }
    >
      {label}
    </NavLink>
  )
}

export default function Layout() {
  const { data: status } = useApi<UpsStatus>('/api/status', 5000)
  const { data: version } = useApi<{ daemon: string }>('/api/version')

  useEffect(() => {
    document.title = status?.name
      ? `${status.name} — airies-ups`
      : 'airies-ups'
  }, [status?.name])

  return (
    <div className="flex min-h-screen bg-page text-primary">
      <aside className="w-52 border-r border-edge px-3 py-4 flex flex-col gap-0.5 shrink-0">
        <div className="px-3 py-2 mb-3">
          <h1 className="text-lg font-semibold tracking-tight">
            {status?.name || 'airies-ups'}
          </h1>
          <div className="flex items-center gap-1.5 mt-1">
            <span className={`w-2 h-2 rounded-full ${
              status?.connected ? 'bg-green-400' : 'bg-faint'
            }`} />
            <span className="text-xs text-muted">
              {status?.connected ? `${status.driver} connected` : 'disconnected'}
            </span>
          </div>
        </div>

        {nav.map((n) => (
          <SideLink key={n.to} to={n.to} label={n.label} />
        ))}

        <p className="text-[10px] text-faint uppercase tracking-widest px-3 mt-5 mb-1">
          Configuration
        </p>
        {configNav.map((n) => (
          <SideLink key={n.to} {...n} />
        ))}

        <div className="mt-auto px-3 py-2 space-y-2">
          <button onClick={async () => {
            try { await apiPost('/api/auth/logout', {}) } catch {}
            localStorage.removeItem('auth_token')
            window.location.href = '/login'
          }}
            className="block w-full text-left text-xs text-faint hover:text-muted transition-colors">
            Logout
          </button>
          <span className="text-[10px] text-faint font-mono">
            UI {__APP_VERSION__}{version?.daemon ? ` · Daemon ${version.daemon}` : ''}
          </span>
        </div>
      </aside>

      <main className="flex-1 p-6 overflow-auto min-w-0">
        <Outlet />
      </main>
    </div>
  )
}
