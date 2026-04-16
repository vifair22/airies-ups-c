import { useState } from 'react'
import { useNavigate } from 'react-router-dom'
import { apiPostPublic } from '../hooks/useApi'

interface LoginResult {
  token?: string
  error?: string
}

export default function Login() {
  const [password, setPassword] = useState('')
  const [error, setError] = useState('')
  const [loading, setLoading] = useState(false)
  const navigate = useNavigate()

  const submit = async (e: React.FormEvent) => {
    e.preventDefault()
    setLoading(true)
    setError('')

    const res = await apiPostPublic<LoginResult>('/api/auth/login', { password })
    if (res.token) {
      localStorage.setItem('auth_token', res.token)
      navigate('/')
    } else {
      setError(res.error || 'Invalid password')
    }
    setLoading(false)
  }

  return (
    <div className="min-h-screen flex items-center justify-center bg-page">
      <div className="w-full max-w-sm">
        <h1 className="text-2xl font-bold text-center mb-6">airies-ups</h1>
        <form onSubmit={submit} className="rounded-lg bg-panel border border-edge p-6 space-y-4">
          <div>
            <label className="text-xs text-muted">Admin Password</label>
            <input type="password" value={password}
              onChange={(e) => setPassword(e.target.value)}
              autoFocus
              className="block w-full bg-field border border-edge-strong rounded px-3 py-2 text-sm mt-1" />
          </div>
          {error && <p className="text-sm text-red-400">{error}</p>}
          <button type="submit" disabled={loading || !password}
            className="w-full px-4 py-2 bg-accent hover:bg-accent-hover rounded text-sm disabled:opacity-50">
            {loading ? 'Logging in...' : 'Login'}
          </button>
        </form>
      </div>
    </div>
  )
}
