import { useState, useEffect, useCallback } from 'react'
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

interface SetupStatus {
  needs_setup: boolean
  password_set: boolean
  ups_configured: boolean
  ups_connected: boolean
}

type Step = 'password' | 'login' | 'connection' | 'notifications' | 'done'

/* Visual progress only shows the 4 main phases */
const PROGRESS_STEPS = ['auth', 'connection', 'notifications', 'done'] as const
const STEP_TO_PROGRESS: Record<Step, number> = {
  password: 0, login: 0, connection: 1, notifications: 2, done: 3,
}

export default function Setup() {
  const navigate = useNavigate()
  const [loading, setLoading] = useState(true)
  const [step, setStep] = useState<Step>('password')

  /* Step 1: password */
  const [password, setPassword] = useState('')
  const [passwordConfirm, setPasswordConfirm] = useState('')
  const [passwordError, setPasswordError] = useState('')
  const [passwordSaving, setPasswordSaving] = useState(false)

  /* Step 2: connection */
  interface UsbDevice { vid: string; pid: string; name: string; device: string }
  interface PortScan { serial: string[]; usb: UsbDevice[] }
  const [connType, setConnType] = useState('serial')
  const [portScan, setPortScan] = useState<PortScan>({ serial: [], usb: [] })
  const [device, setDevice] = useState('/dev/ttyUSB0')
  const [baud, setBaud] = useState(9600)
  const [slaveId, setSlaveId] = useState(1)
  const [usbVid, setUsbVid] = useState('051d')
  const [usbPid, setUsbPid] = useState('0002')
  const [upsName, setUpsName] = useState('')
  const [testResult, setTestResult] = useState<TestResult | null>(null)
  const [testing, setTesting] = useState(false)

  /* Step 3: Pushover */
  const [pushToken, setPushToken] = useState('')
  const [pushUser, setPushUser] = useState('')
  const [saving, setSaving] = useState(false)
  const [saveError, setSaveError] = useState('')
  const [restarting, setRestarting] = useState(false)

  /* Determine starting step based on what's already configured */
  useEffect(() => {
    fetch('/api/setup/status')
      .then(r => r.json())
      .then((data: SetupStatus) => {
        if (data.password_set && data.ups_configured) {
          navigate('/', { replace: true })
        } else if (data.password_set) {
          const token = localStorage.getItem('auth_token')
          setStep(token ? 'connection' : 'login')
        }
      })
      .catch(() => { /* start at password */ })
      .finally(() => setLoading(false))
  }, [navigate])

  /* Load available ports/devices when entering connection step */
  useEffect(() => {
    if (step === 'connection') {
      fetch('/api/setup/ports').then(r => r.json()).then((data: PortScan) => {
        setPortScan(data)
        /* Auto-select USB if a UPS-like device is detected and no serial ports */
        if (data.usb.length > 0 && data.serial.length === 0) {
          setConnType('usb')
          setUsbVid(data.usb[0].vid)
          setUsbPid(data.usb[0].pid)
        } else if (data.serial.length > 0) {
          setDevice(data.serial[0])
        }
      }).catch(() => {})
    }
  }, [step])

  const submitLogin = useCallback(async () => {
    if (!password) return
    setPasswordError('')
    setPasswordSaving(true)
    try {
      const res = await apiPostPublic<{ token?: string; error?: string }>('/api/auth/login', { password })
      if (res.error) { setPasswordError(res.error); return }
      if (res.token) localStorage.setItem('auth_token', res.token)
      setStep('connection')
    } catch {
      setPasswordError('Failed to connect to server')
    } finally {
      setPasswordSaving(false)
    }
  }, [password])

  const submitPassword = useCallback(async () => {
    if (password.length < 4) { setPasswordError('Password must be at least 4 characters'); return }
    if (password !== passwordConfirm) { setPasswordError('Passwords do not match'); return }
    setPasswordError('')
    setPasswordSaving(true)

    try {
      const res = await apiPostPublic<{ result?: string; error?: string }>('/api/auth/setup', { password })
      if (res.error) { setPasswordError(res.error); return }

      const login = await apiPostPublic<{ token?: string }>('/api/auth/login', { password })
      if (login.token) localStorage.setItem('auth_token', login.token)

      setStep('connection')
    } catch {
      setPasswordError('Failed to connect to server')
    } finally {
      setPasswordSaving(false)
    }
  }, [password, passwordConfirm])

  const testConnection = useCallback(async () => {
    setTesting(true)
    setTestResult(null)
    try {
      const body = connType === 'usb'
        ? { conn_type: 'usb', usb_vid: usbVid, usb_pid: usbPid }
        : { conn_type: 'serial', device, baud, slave_id: slaveId }
      const res = await apiPostPublic<TestResult>('/api/setup/test', body)
      setTestResult(res)
    } catch {
      setTestResult({ error: 'Failed to connect to server' })
    } finally {
      setTesting(false)
    }
  }, [connType, usbVid, usbPid, device, baud, slaveId])

  const saveConfig = useCallback(async () => {
    setSaving(true)
    setSaveError('')

    try {
      const token = localStorage.getItem('auth_token')
      const headers: Record<string, string> = { 'Content-Type': 'application/json' }
      if (token) headers['Authorization'] = `Bearer ${token}`

      const setConfig = async (key: string, value: string) => {
        const res = await fetch('/api/config/app', {
          method: 'POST', headers,
          body: JSON.stringify({ key, value }),
        })
        if (!res.ok) throw new Error(`Failed to save ${key}`)
      }

      if (upsName) await setConfig('setup.ups_name', upsName)
      await setConfig('ups.conn_type', connType)
      if (connType === 'usb') {
        await setConfig('ups.usb_vid', usbVid)
        await setConfig('ups.usb_pid', usbPid)
      } else {
        await setConfig('ups.device', device)
        await setConfig('ups.baud', String(baud))
        await setConfig('ups.slave_id', String(slaveId))
      }

      if (pushToken && pushUser) {
        await setConfig('pushover.token', pushToken)
        await setConfig('pushover.user', pushUser)
      }

      await setConfig('setup.ups_done', '1')

      setStep('done')
    } catch (e) {
      setSaveError(e instanceof Error ? e.message : 'Failed to save configuration')
    } finally {
      setSaving(false)
    }
  }, [connType, usbVid, usbPid, device, baud, slaveId, upsName, pushToken, pushUser])

  const restart = useCallback(async () => {
    setRestarting(true)
    const token = localStorage.getItem('auth_token')
    const headers: Record<string, string> = { 'Content-Type': 'application/json' }
    if (token) headers['Authorization'] = `Bearer ${token}`

    await fetch('/api/restart', { method: 'POST', headers }).catch(() => {})

    const poll = () => {
      fetch('/api/setup/status')
        .then(r => { if (r.ok) navigate('/'); else setTimeout(poll, 500) })
        .catch(() => setTimeout(poll, 500))
    }
    setTimeout(poll, 1500)
  }, [navigate])

  const handleKeyDown = useCallback((action: () => void) => {
    return (e: React.KeyboardEvent) => {
      if (e.key === 'Enter') { e.preventDefault(); action() }
    }
  }, [])

  if (loading) {
    return (
      <div className="min-h-screen flex items-center justify-center bg-page">
        <div className="h-8 w-8 border-2 border-accent border-t-transparent rounded-full animate-spin" />
      </div>
    )
  }

  const progressIdx = STEP_TO_PROGRESS[step]

  return (
    <div className="min-h-screen flex items-center justify-center bg-page">
      <div className="w-full max-w-lg">
        <h1 className="text-2xl font-bold text-center mb-2">airies-ups</h1>
        <p className="text-sm text-muted text-center mb-6">Initial Setup</p>

        {/* Progress */}
        <div className="flex items-center justify-center gap-2 mb-6">
          {PROGRESS_STEPS.map((s, i) => (
            <div key={s} className={`w-8 h-1 rounded-full ${i <= progressIdx ? 'bg-accent' : 'bg-field'}`} />
          ))}
        </div>

        <div className="rounded-lg bg-panel border border-edge p-6">
          <p className="text-xs text-muted mb-4">Step {progressIdx + 1} of {PROGRESS_STEPS.length}</p>

          {/* Step 1: Password */}
          {step === 'password' && (
            <div className="space-y-4" onKeyDown={handleKeyDown(submitPassword)}>
              <h2 className="text-lg font-semibold">Set Admin Password</h2>
              <p className="text-sm text-secondary">This password protects access to the web UI and API.</p>
              <div>
                <label className="text-xs text-muted">Password</label>
                <input type="password" value={password}
                  onChange={(e) => setPassword(e.target.value)} autoFocus
                  className="block w-full bg-field border border-edge-strong rounded px-3 py-2 text-sm mt-1" />
              </div>
              <div>
                <label className="text-xs text-muted">Confirm Password</label>
                <input type="password" value={passwordConfirm}
                  onChange={(e) => setPasswordConfirm(e.target.value)}
                  className="block w-full bg-field border border-edge-strong rounded px-3 py-2 text-sm mt-1" />
              </div>
              {passwordError && <p className="text-sm text-red-400">{passwordError}</p>}
              <button onClick={submitPassword} disabled={!password || !passwordConfirm || passwordSaving}
                className="w-full px-4 py-2 bg-accent hover:bg-accent-hover text-white rounded text-sm disabled:opacity-50">
                {passwordSaving ? 'Setting up...' : 'Continue'}
              </button>
            </div>
          )}

          {/* Login (password already set, need token) */}
          {step === 'login' && (
            <div className="space-y-4" onKeyDown={handleKeyDown(submitLogin)}>
              <h2 className="text-lg font-semibold">Welcome Back</h2>
              <p className="text-sm text-secondary">Your admin password is already set. Log in to continue setup.</p>
              <div>
                <label className="text-xs text-muted">Password</label>
                <input type="password" value={password}
                  onChange={(e) => setPassword(e.target.value)} autoFocus
                  className="block w-full bg-field border border-edge-strong rounded px-3 py-2 text-sm mt-1" />
              </div>
              {passwordError && <p className="text-sm text-red-400">{passwordError}</p>}
              <button onClick={submitLogin} disabled={!password || passwordSaving}
                className="w-full px-4 py-2 bg-accent hover:bg-accent-hover text-white rounded text-sm disabled:opacity-50">
                {passwordSaving ? 'Logging in...' : 'Continue'}
              </button>
            </div>
          )}

          {/* Step 2: UPS Connection */}
          {step === 'connection' && (
            <div className="space-y-4">
              <h2 className="text-lg font-semibold">UPS Connection</h2>
              <p className="text-sm text-secondary">Configure and test the connection to your UPS.</p>

              <div>
                <label className="text-xs text-muted">UPS Name</label>
                <input type="text" value={upsName}
                  onChange={(e) => setUpsName(e.target.value)}
                  placeholder="e.g. Server Room, Office, Lab"
                  className="block w-full bg-field border border-edge-strong rounded px-3 py-2 text-sm mt-1" />
                <p className="text-[10px] text-faint mt-1">A friendly name shown in the browser tab and header.</p>
              </div>

              <div>
                <label className="text-xs text-muted">Connection Type</label>
                <select value={connType} onChange={(e) => { setConnType(e.target.value); setTestResult(null) }}
                  className="block w-full bg-field border border-edge-strong rounded px-3 py-2 text-sm mt-1">
                  <option value="serial">Serial (Modbus RTU)</option>
                  <option value="usb">USB (HID)</option>
                </select>
              </div>

              {connType === 'serial' && (
                <>
                  <div>
                    <label className="text-xs text-muted">Serial Device</label>
                    {portScan.serial.length > 0 ? (
                      <select value={device} onChange={(e) => setDevice(e.target.value)}
                        className="block w-full bg-field border border-edge-strong rounded px-3 py-2 text-sm mt-1">
                        {portScan.serial.map(p => <option key={p} value={p}>{p}</option>)}
                        <option value="">Manual entry...</option>
                      </select>
                    ) : (
                      <input type="text" value={device} onChange={(e) => setDevice(e.target.value)}
                        className="block w-full bg-field border border-edge-strong rounded px-3 py-2 text-sm mt-1" />
                    )}
                  </div>
                  <div className="grid grid-cols-2 gap-4">
                    <div>
                      <label className="text-xs text-muted">Baud Rate</label>
                      <select value={baud} onChange={(e) => setBaud(parseInt(e.target.value))}
                        className="block w-full bg-field border border-edge-strong rounded px-3 py-2 text-sm mt-1">
                        <option value={9600}>9600</option>
                        <option value={19200}>19200</option>
                        <option value={38400}>38400</option>
                        <option value={115200}>115200</option>
                      </select>
                    </div>
                    <div>
                      <label className="text-xs text-muted">Slave ID</label>
                      <input type="number" value={slaveId} min={1} max={247}
                        onChange={(e) => setSlaveId(parseInt(e.target.value) || 1)}
                        className="block w-full bg-field border border-edge-strong rounded px-3 py-2 text-sm mt-1" />
                    </div>
                  </div>
                </>
              )}

              {connType === 'usb' && (
                <div className="space-y-3">
                  {portScan.usb.length > 0 ? (
                    <div>
                      <label className="text-xs text-muted">Detected USB Devices</label>
                      <select
                        value={`${usbVid}:${usbPid}`}
                        onChange={(e) => {
                          const [v, p] = e.target.value.split(':')
                          setUsbVid(v); setUsbPid(p); setTestResult(null)
                        }}
                        className="block w-full bg-field border border-edge-strong rounded px-3 py-2 text-sm mt-1">
                        {portScan.usb.map((d) => (
                          <option key={d.device} value={`${d.vid}:${d.pid}`}>
                            {d.name} ({d.vid}:{d.pid})
                          </option>
                        ))}
                        <option value=":">Manual entry...</option>
                      </select>
                    </div>
                  ) : (
                    <p className="text-sm text-muted">No USB HID devices detected.</p>
                  )}
                  {(portScan.usb.length === 0 || (usbVid === '' && usbPid === '')) && (
                    <div className="grid grid-cols-2 gap-4">
                      <div>
                        <label className="text-xs text-muted">Vendor ID (hex)</label>
                        <input type="text" value={usbVid} onChange={(e) => setUsbVid(e.target.value)}
                          placeholder="051d"
                          className="block w-full bg-field border border-edge-strong rounded px-3 py-2 text-sm mt-1 font-mono" />
                      </div>
                      <div>
                        <label className="text-xs text-muted">Product ID (hex)</label>
                        <input type="text" value={usbPid} onChange={(e) => setUsbPid(e.target.value)}
                          placeholder="0002"
                          className="block w-full bg-field border border-edge-strong rounded px-3 py-2 text-sm mt-1 font-mono" />
                      </div>
                    </div>
                  )}
                </div>
              )}

              <button onClick={testConnection} disabled={testing || (connType === 'serial' && !device)}
                className="w-full px-4 py-2 bg-accent hover:bg-accent-hover text-white rounded text-sm disabled:opacity-50">
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

              <button onClick={() => setStep('notifications')} disabled={!testResult?.result}
                className="w-full px-4 py-2 bg-accent hover:bg-accent-hover text-white rounded text-sm disabled:opacity-50">
                Continue
              </button>
            </div>
          )}

          {/* Step 3: Pushover (optional) */}
          {step === 'notifications' && (
            <div className="space-y-4" onKeyDown={handleKeyDown(saveConfig)}>
              <h2 className="text-lg font-semibold">Notifications</h2>
              <p className="text-sm text-secondary">Optional: configure Pushover for real-time alerts. You can set this up later in App Settings.</p>
              <div>
                <label className="text-xs text-muted">Pushover API Token</label>
                <input type="text" value={pushToken}
                  onChange={(e) => setPushToken(e.target.value)}
                  placeholder="Leave blank to skip" autoFocus
                  className="block w-full bg-field border border-edge-strong rounded px-3 py-2 text-sm mt-1" />
              </div>
              <div>
                <label className="text-xs text-muted">Pushover User Key</label>
                <input type="text" value={pushUser}
                  onChange={(e) => setPushUser(e.target.value)}
                  placeholder="Leave blank to skip"
                  className="block w-full bg-field border border-edge-strong rounded px-3 py-2 text-sm mt-1" />
              </div>
              {saveError && <p className="text-sm text-red-400">{saveError}</p>}
              <div className="flex gap-3">
                <button onClick={() => setStep('connection')}
                  className="px-4 py-2 bg-field hover:bg-field-hover rounded text-sm border border-edge-strong">
                  Back
                </button>
                <button onClick={saveConfig} disabled={saving}
                  className="flex-1 px-4 py-2 bg-accent hover:bg-accent-hover text-white rounded text-sm disabled:opacity-50">
                  {saving ? 'Saving...' : pushToken ? 'Save & Finish' : 'Skip & Finish'}
                </button>
              </div>
            </div>
          )}

          {/* Step 4: Done */}
          {step === 'done' && !restarting && (
            <div className="space-y-4 text-center py-4">
              <h2 className="text-lg font-semibold text-green-400">Setup Complete</h2>
              <p className="text-sm text-secondary">
                Configuration saved. Restart the daemon to connect to the UPS with the new settings.
              </p>
              <button onClick={restart}
                className="px-6 py-2 bg-accent hover:bg-accent-hover text-white rounded text-sm">
                Restart & Go to Dashboard
              </button>
            </div>
          )}
          {step === 'done' && restarting && (
            <div className="space-y-4 text-center py-6">
              <div className="h-8 w-8 mx-auto border-2 border-accent border-t-transparent rounded-full animate-spin" />
              <h2 className="text-lg font-semibold">Restarting...</h2>
              <p className="text-sm text-secondary">Waiting for the daemon to come back up.</p>
            </div>
          )}
        </div>
      </div>
    </div>
  )
}
