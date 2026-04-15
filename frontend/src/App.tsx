import { Routes, Route } from 'react-router-dom'
import Layout from './components/Layout'
import Dashboard from './pages/Dashboard'
import Events from './pages/Events'
import Telemetry from './pages/Telemetry'
import Commands from './pages/Commands'
import UpsConfig from './pages/UpsConfig'
import AppConfig from './pages/AppConfig'
import ShutdownConfig from './pages/ShutdownConfig'
import WeatherConfig from './pages/WeatherConfig'

export default function App() {
  return (
    <Routes>
      <Route element={<Layout />}>
        <Route path="/" element={<Dashboard />} />
        <Route path="/events" element={<Events />} />
        <Route path="/telemetry" element={<Telemetry />} />
        <Route path="/commands" element={<Commands />} />
        <Route path="/config/ups" element={<UpsConfig />} />
        <Route path="/config/app" element={<AppConfig />} />
        <Route path="/config/shutdown" element={<ShutdownConfig />} />
        <Route path="/config/weather" element={<WeatherConfig />} />
      </Route>
    </Routes>
  )
}
