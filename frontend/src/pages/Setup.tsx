import { useState, useEffect } from 'react'
import { useNavigate } from 'react-router-dom'
import { apiPostPublic } from '../hooks/useApi'

interface TestResult {
  result?: string
  error?: string
  driver?: string
  topology?: string
  inventory?: {
    model: string
    serial: string
    firmware: string
    nominal_va: number
    nominal_watts: number
  }
}

export default function Setup() {
  const navigate = useNavigate()
  const [step, setStep] = useState(1)

  /* Step 1: password */
  const [password, setPassword] = useState('')
  const [passwordConfirm, setPasswordConfirm] = useState('')
  const [passwordError, setPasswordError] = useState('')

  /* Step 2: serial port */
  const [ports, setPorts] = useState<string[]>([])
  const [device, setDevice] = useState('/dev/ttyUSB0')
  const [baud, setBaud] = useState(9600)
  const [slaveId, setSlaveId] = useState(1)

  /* Step 3: test result */
  const [testResult, setTestResult] = useState<TestResult | null>(null)
  const [testing, setTesting] = useState(false)

  /* Step 4: Pushover */
  const [pushToken, setPushToken] = useState('')
  const [pushUser, setPushUser] = useState('')

  /* Load available ports */
  useEffect(() => {
    fetch('/api/setup/ports').then(r => r.json()).then(setPorts).catch(() => {})
  }, [])

  const submitPassword = async () => {
    if (password.length < 4) { setPasswordError('Password must be at least 4 characters'); return }
    if (password !== passwordConfirm) { setPasswordError('Passwords do not match'); return }
    setPasswordError('')

    const res = await apiPostPublic<{ result?: string; error?: string }>('/api/auth/setup', { password })
    if (res.error) { setPasswordError(res.error); return }

    /* Store a login token immediately */
    const login = await apiPostPublic<{ token?: string }>('/api/auth/login', { password })
    if (login.token) localStorage.setItem('auth_token', login.token)

    setStep(2)
  }

  const testConnection = async () => {
    setTesting(true)
    setTestResult(null)
    const res = await apiPostPublic<TestResult>('/api/setup/test', { device, baud, slave_id: slaveId })
    setTestResult(res)
    setTesting(false)
  }

  const saveConfig = async () => {
    /* Save serial port config */
    const token = localStorage.getItem('auth_token')
    const headers: Record<string, string> = { 'Content-Type': 'application/json' }
    if (token) headers['Authorization'] = `Bearer ${token}`

    await fetch('/api/config/app', {
      method: 'POST', headers,
      body: JSON.stringify({ key: 'ups.device', value: device }),
    })
    await fetch('/api/config/app', {
      method: 'POST', headers,
      body: JSON.stringify({ key: 'ups.baud', value: String(baud) }),
    })
    await fetch('/api/config/app', {
      method: 'POST', headers,
      body: JSON.stringify({ key: 'ups.slave_id', value: String(slaveId) }),
    })

    /* Save Pushover if configured */
    if (pushToken && pushUser) {
      await fetch('/api/config/app', {
        method: 'POST', headers,
        body: JSON.stringify({ key: 'pushover.token', value: pushToken }),
      })
      await fetch('/api/config/app', {
        method: 'POST', headers,
        body: JSON.stringify({ key: 'pushover.user', value: pushUser }),
      })
    }

    setStep(5)
  }

  const restart = async () => {
    const token = localStorage.getItem('auth_token')
    const headers: Record<string, string> = { 'Content-Type': 'application/json' }
    if (token) headers['Authorization'] = `Bearer ${token}`

    await fetch('/api/restart', { method: 'POST', headers })

    /* Wait for daemon to come back up */
    setTimeout(() => navigate('/'), 4000)
  }

  return (
    <div className="min-h-screen flex items-center justify-center bg-gray-950">
      <div className="w-full max-w-lg">
        <h1 className="text-2xl font-bold text-center mb-2">airies-ups</h1>
        <p className="text-sm text-gray-500 text-center mb-6">Initial Setup</p>

        {/* Progress */}
        <div className="flex items-center justify-center gap-2 mb-6">
          {[1,2,3,4,5].map(s => (
            <div key={s} className={`w-8 h-1 rounded-full ${s <= step ? 'bg-blue-500' : 'bg-gray-800'}`} />
          ))}
        </div>

        <div className="rounded-lg bg-gray-900 border border-gray-800 p-6">

          {/* Step 1: Password */}
          {step === 1 && (
            <div className="space-y-4">
              <h2 className="text-lg font-semibold">Set Admin Password</h2>
              <p className="text-sm text-gray-400">This password protects access to the web UI and API.</p>
              <div>
                <label className="text-xs text-gray-400">Password</label>
                <input type="password" value={password}
                  onChange={(e) => setPassword(e.target.value)} autoFocus
                  className="block w-full bg-gray-800 border border-gray-700 rounded px-3 py-2 text-sm mt-1" />
              </div>
              <div>
                <label className="text-xs text-gray-400">Confirm Password</label>
                <input type="password" value={passwordConfirm}
                  onChange={(e) => setPasswordConfirm(e.target.value)}
                  className="block w-full bg-gray-800 border border-gray-700 rounded px-3 py-2 text-sm mt-1" />
              </div>
              {passwordError && <p className="text-sm text-red-400">{passwordError}</p>}
              <button onClick={submitPassword} disabled={!password || !passwordConfirm}
                className="w-full px-4 py-2 bg-blue-700 hover:bg-blue-600 rounded text-sm disabled:opacity-50">
                Continue
              </button>
            </div>
          )}

          {/* Step 2: Serial Port */}
          {step === 2 && (
            <div className="space-y-4">
              <h2 className="text-lg font-semibold">UPS Connection</h2>
              <p className="text-sm text-gray-400">Configure the serial port for Modbus RTU communication with the UPS.</p>
              <div>
                <label className="text-xs text-gray-400">Serial Device</label>
                {ports.length > 0 ? (
                  <select value={device} onChange={(e) => setDevice(e.target.value)}
                    className="block w-full bg-gray-800 border border-gray-700 rounded px-3 py-2 text-sm mt-1">
                    {ports.map(p => <option key={p} value={p}>{p}</option>)}
                    <option value="">Manual entry...</option>
                  </select>
                ) : (
                  <input type="text" value={device} onChange={(e) => setDevice(e.target.value)}
                    className="block w-full bg-gray-800 border border-gray-700 rounded px-3 py-2 text-sm mt-1" />
                )}
                {device === '' && (
                  <input type="text" placeholder="/dev/ttyUSB0"
                    onChange={(e) => setDevice(e.target.value)}
                    className="block w-full bg-gray-800 border border-gray-700 rounded px-3 py-2 text-sm mt-1" />
                )}
              </div>
              <div className="grid grid-cols-2 gap-4">
                <div>
                  <label className="text-xs text-gray-400">Baud Rate</label>
                  <select value={baud} onChange={(e) => setBaud(parseInt(e.target.value))}
                    className="block w-full bg-gray-800 border border-gray-700 rounded px-3 py-2 text-sm mt-1">
                    <option value={9600}>9600</option>
                    <option value={19200}>19200</option>
                    <option value={38400}>38400</option>
                    <option value={115200}>115200</option>
                  </select>
                </div>
                <div>
                  <label className="text-xs text-gray-400">Slave ID</label>
                  <input type="number" value={slaveId} min={1} max={247}
                    onChange={(e) => setSlaveId(parseInt(e.target.value) || 1)}
                    className="block w-full bg-gray-800 border border-gray-700 rounded px-3 py-2 text-sm mt-1" />
                </div>
              </div>
              <button onClick={testConnection} disabled={testing || !device}
                className="w-full px-4 py-2 bg-blue-700 hover:bg-blue-600 rounded text-sm disabled:opacity-50">
                {testing ? 'Testing...' : 'Test Connection'}
              </button>
              {testResult && !testResult.error && (
                <div className="rounded bg-green-900/30 border border-green-700 p-3">
                  <p className="text-sm text-green-300 font-medium">Connected — {testResult.driver?.toUpperCase()} driver</p>
                  {testResult.inventory && (
                    <div className="text-xs text-green-400/80 mt-1 space-y-0.5">
                      <p>Model: {testResult.inventory.model.trim()}</p>
                      <p>Serial: {testResult.inventory.serial.trim()}</p>
                      <p>Rating: {testResult.inventory.nominal_va} VA / {testResult.inventory.nominal_watts} W</p>
                    </div>
                  )}
                </div>
              )}
              {testResult?.error && (
                <p className="text-sm text-red-400">{testResult.error}</p>
              )}
              <div className="flex gap-3">
                <button onClick={() => setStep(1)}
                  className="px-4 py-2 bg-gray-800 hover:bg-gray-700 rounded text-sm border border-gray-700">
                  Back
                </button>
                <button onClick={() => setStep(3)} disabled={!testResult?.result}
                  className="flex-1 px-4 py-2 bg-blue-700 hover:bg-blue-600 rounded text-sm disabled:opacity-50">
                  Continue
                </button>
              </div>
            </div>
          )}

          {/* Step 3: Pushover (optional) */}
          {step === 3 && (
            <div className="space-y-4">
              <h2 className="text-lg font-semibold">Notifications</h2>
              <p className="text-sm text-gray-400">Optional: configure Pushover for real-time alerts. You can set this up later in App Settings.</p>
              <div>
                <label className="text-xs text-gray-400">Pushover API Token</label>
                <input type="text" value={pushToken}
                  onChange={(e) => setPushToken(e.target.value)}
                  placeholder="Leave blank to skip"
                  className="block w-full bg-gray-800 border border-gray-700 rounded px-3 py-2 text-sm mt-1" />
              </div>
              <div>
                <label className="text-xs text-gray-400">Pushover User Key</label>
                <input type="text" value={pushUser}
                  onChange={(e) => setPushUser(e.target.value)}
                  placeholder="Leave blank to skip"
                  className="block w-full bg-gray-800 border border-gray-700 rounded px-3 py-2 text-sm mt-1" />
              </div>
              <div className="flex gap-3">
                <button onClick={() => setStep(2)}
                  className="px-4 py-2 bg-gray-800 hover:bg-gray-700 rounded text-sm border border-gray-700">
                  Back
                </button>
                <button onClick={() => { saveConfig(); setStep(4) }}
                  className="flex-1 px-4 py-2 bg-blue-700 hover:bg-blue-600 rounded text-sm">
                  {pushToken ? 'Save & Continue' : 'Skip & Continue'}
                </button>
              </div>
            </div>
          )}

          {/* Step 4: Saving */}
          {step === 4 && (
            <div className="space-y-4 text-center py-4">
              <h2 className="text-lg font-semibold">Saving Configuration</h2>
              <div className="h-2 w-48 mx-auto bg-gray-800 rounded-full overflow-hidden">
                <div className="h-full bg-blue-500 rounded-full animate-pulse" style={{ width: '60%' }} />
              </div>
              <p className="text-sm text-gray-400">Writing config...</p>
            </div>
          )}

          {/* Step 5: Done */}
          {step === 5 && (
            <div className="space-y-4 text-center py-4">
              <h2 className="text-lg font-semibold text-green-400">Setup Complete</h2>
              <p className="text-sm text-gray-400">
                Configuration saved. Restart the daemon to connect to the UPS with the new settings.
              </p>
              <button onClick={restart}
                className="px-6 py-2 bg-blue-700 hover:bg-blue-600 rounded text-sm">
                Restart & Go to Dashboard
              </button>
            </div>
          )}
        </div>
      </div>
    </div>
  )
}
