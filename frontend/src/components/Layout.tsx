import { NavLink, Outlet } from 'react-router-dom'

const nav = [
  { to: '/', label: 'Dashboard' },
  { to: '/events', label: 'Events' },
  { to: '/telemetry', label: 'Telemetry' },
  { to: '/commands', label: 'Commands' },
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
        `block px-4 py-2 rounded text-sm ${
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
  return (
    <div className="flex min-h-screen bg-gray-950 text-gray-100">
      <aside className="w-52 border-r border-gray-800 p-4 flex flex-col gap-1">
        <h1 className="text-lg font-semibold px-4 py-2 mb-2">airies-ups</h1>

        {nav.map((n) => (
          <SideLink key={n.to} {...n} />
        ))}

        <p className="text-xs text-gray-600 uppercase tracking-wider px-4 mt-4 mb-1">
          Config
        </p>
        {configNav.map((n) => (
          <SideLink key={n.to} {...n} />
        ))}

        <div className="mt-auto px-4 py-2 text-xs text-gray-600">v0.1.0</div>
      </aside>

      <main className="flex-1 p-6 overflow-auto">
        <Outlet />
      </main>
    </div>
  )
}
