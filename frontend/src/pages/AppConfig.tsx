import { useState } from 'react'
import { useApi, apiPost } from '../hooks/useApi'
import { useTheme } from '../hooks/useTheme'
import type { ThemeMode } from '../hooks/useTheme'
import type { ConfigEntry } from '../types/config'

export default function AppConfig() {
  const { data: config, error, loading, refetch } = useApi<ConfigEntry[]>('/api/config/app')
  const [saving, setSaving] = useState<string | null>(null)
  const [recentlySaved, setRecentlySaved] = useState<string | null>(null)
  const [restarting, setRestarting] = useState(false)

  if (loading) return <p className="text-muted">Loading...</p>
  if (error) return <p className="text-red-400">{error}</p>
  if (!config) return null

  const handleSave = async (key: string, value: string) => {
    setSaving(key)
    try {
      await apiPost('/api/config/app', { key, value })
      setRestarting(true)
      await apiPost('/api/restart', {})
      const poll = () => {
        fetch('/api/status')
          .then(r => {
            if (r.ok) {
              setRestarting(false)
              setSaving(null)
              setRecentlySaved(key)
              setTimeout(() => setRecentlySaved((cur) => (cur === key ? null : cur)), 2000)
              refetch()
            } else {
              setTimeout(poll, 500)
            }
          })
          .catch(() => setTimeout(poll, 500))
      }
      setTimeout(poll, 1500)
    } catch {
      alert('Failed to save')
      setSaving(null)
    }
  }

  const groups = [...new Set(config.map((c) => c.key.split('.')[0]))]

  return (
    <div>
      <h2 className="text-xl font-semibold mb-1">App Settings</h2>
      <p className="text-sm text-muted mb-4">
        {restarting ? 'Restarting daemon...' : 'Changes are saved and applied automatically.'}
      </p>

      <ThemeSelector />
      <PasswordChange />

      {groups.map((group) => (
        <div key={group} className="mb-4">
          <div className="rounded-lg bg-panel border border-edge">
            <div className="px-4 py-2.5 border-b border-edge">
              <h3 className="text-xs font-medium text-muted uppercase tracking-wider">{group}</h3>
            </div>
            <div className="divide-y divide-edge">
              {config
                .filter((c) => c.key.startsWith(group + '.'))
                .map((c) => (
                  <ConfigRow key={c.key} entry={c} saving={saving}
                             recentlySaved={recentlySaved} onSave={handleSave} />
                ))}
            </div>
          </div>
        </div>
      ))}
    </div>
  )
}

function ThemeSelector() {
  const { mode, setMode } = useTheme()
  return (
    <div className="mb-4 rounded-lg bg-panel border border-edge">
      <div className="px-4 py-2.5 border-b border-edge">
        <h3 className="text-xs font-medium text-muted uppercase tracking-wider">Appearance</h3>
      </div>
      <div className="px-4 py-3 flex items-center gap-4">
        <div className="flex-1 min-w-0">
          <label className="text-sm text-primary font-medium">Theme</label>
          <p className="text-xs text-muted">Auto follows your system preference</p>
        </div>
        <select value={mode} onChange={(e) => setMode(e.target.value as ThemeMode)}
          className="bg-field border border-edge-strong rounded px-3 py-1.5 text-sm">
          <option value="auto">Auto</option>
          <option value="dark">Dark</option>
          <option value="light">Light</option>
        </select>
      </div>
    </div>
  )
}

function PasswordChange() {
  const [oldPw, setOldPw] = useState('')
  const [newPw, setNewPw] = useState('')
  const [confirmPw, setConfirmPw] = useState('')
  const [msg, setMsg] = useState<{ text: string; ok: boolean } | null>(null)
  const [saving, setSaving] = useState(false)

  const submit = async () => {
    if (newPw.length < 4) { setMsg({ text: 'New password must be at least 4 characters', ok: false }); return }
    if (newPw !== confirmPw) { setMsg({ text: 'Passwords do not match', ok: false }); return }
    setSaving(true)
    setMsg(null)
    try {
      const res = await apiPost<{ result?: string; error?: string }>('/api/auth/change', {
        old_password: oldPw, new_password: newPw,
      })
      if (res.error) {
        setMsg({ text: res.error, ok: false })
      } else {
        setMsg({ text: 'Password changed', ok: true })
        setOldPw(''); setNewPw(''); setConfirmPw('')
      }
    } catch {
      setMsg({ text: 'Failed to change password', ok: false })
    }
    setSaving(false)
    setTimeout(() => setMsg(null), 4000)
  }

  return (
    <div className="mb-4 rounded-lg bg-panel border border-edge">
      <div className="px-4 py-2.5 border-b border-edge">
        <h3 className="text-xs font-medium text-muted uppercase tracking-wider">Admin Password</h3>
      </div>
      <div className="px-4 py-3 space-y-3">
        <div className="grid grid-cols-1 sm:grid-cols-3 gap-3">
          <div>
            <label className="text-xs text-muted">Current Password</label>
            <input type="password" value={oldPw} onChange={(e) => setOldPw(e.target.value)}
              className="block w-full bg-field border border-edge-strong rounded px-3 py-1.5 text-sm mt-1" />
          </div>
          <div>
            <label className="text-xs text-muted">New Password</label>
            <input type="password" value={newPw} onChange={(e) => setNewPw(e.target.value)}
              className="block w-full bg-field border border-edge-strong rounded px-3 py-1.5 text-sm mt-1" />
          </div>
          <div>
            <label className="text-xs text-muted">Confirm New Password</label>
            <input type="password" value={confirmPw} onChange={(e) => setConfirmPw(e.target.value)}
              className="block w-full bg-field border border-edge-strong rounded px-3 py-1.5 text-sm mt-1" />
          </div>
        </div>
        <div className="flex items-center gap-3">
          <button onClick={submit} disabled={saving || !oldPw || !newPw || !confirmPw}
            className="px-4 py-1.5 rounded text-sm bg-accent hover:bg-accent-hover text-white disabled:bg-field disabled:text-muted disabled:cursor-not-allowed">
            {saving ? 'Saving...' : 'Change Password'}
          </button>
          {msg && <span className={`text-xs ${msg.ok ? 'text-green-400' : 'text-red-400'}`}>{msg.text}</span>}
        </div>
      </div>
    </div>
  )
}

function ConfigRow({ entry, saving, recentlySaved, onSave }: {
  entry: ConfigEntry
  saving: string | null
  recentlySaved: string | null
  onSave: (key: string, value: string) => void
}) {
  const [val, setVal] = useState(entry.value)
  const changed = val !== entry.value
  const subKey = entry.key.split('.').slice(1).join('.')
  const isDefault = entry.value === entry.default_value
  const isSaving = saving === entry.key
  const justSaved = recentlySaved === entry.key

  return (
    <div className="flex flex-col sm:flex-row sm:items-center gap-2 sm:gap-4 px-4 py-3">
      <div className="flex-1 min-w-0">
        <div className="flex items-center gap-2">
          <label className="text-sm text-primary font-medium">{subKey}</label>
          {!isDefault && (
            <span className="text-[10px] text-yellow-600">modified</span>
          )}
        </div>
        <p className="text-xs text-muted">{entry.description}</p>
        {entry.default_value && (
          <p className="text-[10px] text-faint mt-0.5">default: {entry.default_value}</p>
        )}
      </div>
      <div className="flex items-center gap-2 sm:gap-4">
        <input
          type={entry.type === 'int' ? 'number' : 'text'}
          value={val}
          onChange={(e) => setVal(e.target.value)}
          onBlur={() => { if (changed && !isSaving) onSave(entry.key, val) }}
          className="flex-1 sm:flex-none sm:w-28 bg-field border border-edge-strong rounded px-3 py-1.5 text-sm text-right font-mono"
        />
        <span className="text-xs w-20 text-right shrink-0">
          {isSaving ? <span className="text-muted animate-pulse">saving…</span>
            : justSaved ? <span className="text-green-400">saved ✓</span>
            : null}
        </span>
      </div>
    </div>
  )
}
