import { Routes, Route, Navigate, useLocation } from 'react-router-dom'
import { useState, useEffect, lazy, Suspense } from 'react'
import { useTheme } from './hooks/useTheme'
import Layout from './components/Layout'
import Dashboard from './pages/Dashboard'
import Events from './pages/Events'
import Commands from './pages/Commands'
import UpsConfig from './pages/UpsConfig'
import AppConfig from './pages/AppConfig'
import ShutdownConfig from './pages/ShutdownConfig'
import WeatherConfig from './pages/WeatherConfig'
import About from './pages/About'
import Login from './pages/Login'
import Setup from './pages/Setup'

const DevPowerFlow = import.meta.env.DEV
  ? lazy(() => import('./pages/DevPowerFlow'))
  : null

function AuthGuard({ children }: { children: React.ReactNode }) {
  const location = useLocation()
  const [state, setState] = useState<'loading' | 'setup' | 'login' | 'ok'>('loading')

  useEffect(() => {
    fetch('/api/setup/status')
      .then(r => r.json())
      .then(data => {
        if (data.needs_setup) {
          setState('setup')
        } else if (!localStorage.getItem('auth_token')) {
          setState('login')
        } else {
          setState('ok')
        }
      })
      .catch(() => setState('ok'))
  }, [location.pathname])

  if (state === 'loading') {
    return (
      <div className="min-h-screen flex items-center justify-center bg-page">
        <div className="h-8 w-8 border-2 border-accent border-t-transparent rounded-full animate-spin" />
      </div>
    )
  }
  if (state === 'setup') return <Navigate to="/setup" replace />
  if (state === 'login') return <Navigate to="/login" replace />
  return <>{children}</>
}

export default function App() {
  useTheme() /* Apply saved theme on mount */
  return (
    <Routes>
      {/* Public routes */}
      <Route path="/login" element={<Login />} />
      <Route path="/setup" element={<Setup />} />

      {/* Dev-only routes — eliminated from prod builds via import.meta.env.DEV */}
      {DevPowerFlow && (
        <Route path="/dev/powerflow" element={
          <Suspense fallback={<div className="min-h-screen flex items-center justify-center bg-page">
            <div className="h-8 w-8 border-2 border-accent border-t-transparent rounded-full animate-spin" />
          </div>}>
            <DevPowerFlow />
          </Suspense>
        } />
      )}

      {/* Protected routes */}
      <Route element={
        <AuthGuard>
          <Layout />
        </AuthGuard>
      }>
        <Route path="/" element={<Dashboard />} />
        <Route path="/events" element={<Events />} />
        <Route path="/commands" element={<Commands />} />
        <Route path="/about" element={<About />} />
        <Route path="/config/ups" element={<UpsConfig />} />
        <Route path="/config/app" element={<AppConfig />} />
        <Route path="/config/shutdown" element={<ShutdownConfig />} />
        <Route path="/config/weather" element={<WeatherConfig />} />
      </Route>
    </Routes>
  )
}
