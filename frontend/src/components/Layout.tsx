import { useEffect, useState } from 'react'
import { NavLink, Outlet, useLocation } from 'react-router-dom'
import { useApi, apiPost } from '../hooks/useApi'
import type { UpsStatus } from '../types/ups'

const nav = [
  { to: '/', label: 'Dashboard', icon: '~' },
  { to: '/events', label: 'Events', icon: '!' },
  { to: '/commands', label: 'Commands', icon: '>' },
  { to: '/about', label: 'About', icon: '?' },
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
        `block px-3 py-3 md:py-1.5 rounded text-sm transition-colors ${
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
  const [drawerOpen, setDrawerOpen] = useState(false)
  const location = useLocation()

  useEffect(() => {
    document.title = status?.name
      ? `${status.name} — airies-ups`
      : 'airies-ups'
  }, [status?.name])

  useEffect(() => {
    setDrawerOpen(false)
  }, [location.pathname])

  return (
    <div className="flex min-h-screen bg-page text-primary">
      <header className="md:hidden fixed top-0 inset-x-0 h-12 z-30 bg-panel border-b border-edge flex items-center px-3 gap-3">
        <button
          type="button"
          onClick={() => setDrawerOpen(true)}
          aria-label="Open navigation menu"
          className="p-2 -m-2 text-primary hover:text-muted transition-colors"
        >
          <svg width="20" height="20" viewBox="0 0 20 20" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" aria-hidden="true">
            <line x1="3" y1="6" x2="17" y2="6" />
            <line x1="3" y1="10" x2="17" y2="10" />
            <line x1="3" y1="14" x2="17" y2="14" />
          </svg>
        </button>
        <h1 className="text-base font-semibold tracking-tight truncate">
          {status?.name || 'airies-ups'}
        </h1>
        <span
          className={`w-2 h-2 rounded-full ml-auto shrink-0 ${
            status?.connected ? 'bg-green-400' : 'bg-faint'
          }`}
          aria-label={status?.connected ? 'Connected' : 'Disconnected'}
        />
      </header>

      {drawerOpen && (
        <div
          className="md:hidden fixed inset-0 bg-black/50 z-40"
          onClick={() => setDrawerOpen(false)}
          aria-hidden="true"
        />
      )}

      <aside
        className={`fixed md:static inset-y-0 left-0 z-50 md:z-auto w-52 bg-panel md:bg-transparent border-r border-edge px-3 py-4 flex flex-col gap-0.5 shrink-0 overflow-y-auto transition-transform duration-200 ease-out ${
          drawerOpen ? 'translate-x-0' : '-translate-x-full md:translate-x-0'
        }`}
      >
        <div className="px-3 py-2 mb-3 flex items-start justify-between gap-2">
          <div className="min-w-0">
            <h1 className="text-lg font-semibold tracking-tight truncate">
              {status?.name || 'airies-ups'}
            </h1>
            <div className="flex items-center gap-1.5 mt-1">
              <span className={`w-2 h-2 rounded-full shrink-0 ${
                status?.connected ? 'bg-green-400' : 'bg-faint'
              }`} />
              <span className="text-xs text-muted truncate">
                {status?.connected ? `${status.driver} connected` : 'disconnected'}
              </span>
            </div>
          </div>
          <button
            type="button"
            onClick={() => setDrawerOpen(false)}
            aria-label="Close navigation menu"
            className="md:hidden p-1 -m-1 text-muted hover:text-primary transition-colors shrink-0"
          >
            <svg width="18" height="18" viewBox="0 0 20 20" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" aria-hidden="true">
              <line x1="5" y1="5" x2="15" y2="15" />
              <line x1="15" y1="5" x2="5" y2="15" />
            </svg>
          </button>
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
            className="block w-full text-left text-xs text-faint hover:text-muted transition-colors py-2 md:py-0">
            Logout
          </button>
          <span className="text-[10px] text-faint font-mono">
            UI {__APP_VERSION__}{version?.daemon ? ` · Daemon ${version.daemon}` : ''}
          </span>
        </div>
      </aside>

      <main className="flex-1 pt-16 px-4 pb-4 md:p-6 overflow-auto min-w-0">
        <Outlet />
      </main>
    </div>
  )
}
