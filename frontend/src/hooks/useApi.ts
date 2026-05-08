import { useState, useEffect, useCallback } from 'react'

/* Auth is now cookie-based (HttpOnly auth cookie set by /api/auth/login).
 * `credentials: 'include'` makes fetch send the cookie on every request,
 * including SSE via EventSource (which can't send custom headers). The
 * old localStorage('auth_token') flow is gone — the browser owns the
 * credential, the JS layer can't read it (HttpOnly), and clearing on 401
 * is handled by simply navigating to /login. */

function on401(): void {
  window.location.href = '/login'
}

export function useApi<T>(url: string, interval?: number) {
  const [data, setData] = useState<T | null>(null)
  const [error, setError] = useState<string | null>(null)
  const [loading, setLoading] = useState(true)

  const fetchData = useCallback(async () => {
    try {
      const res = await fetch(url, { credentials: 'include' })
      if (res.status === 401) {
        on401()
        return
      }
      if (!res.ok) throw new Error(`${res.status} ${res.statusText}`)
      const json = await res.json()
      setData(json)
      setError(null)
    } catch (e) {
      setError(e instanceof Error ? e.message : 'Unknown error')
    } finally {
      setLoading(false)
    }
  }, [url])

  useEffect(() => {
    fetchData()
    if (interval) {
      const id = setInterval(fetchData, interval)
      return () => clearInterval(id)
    }
  }, [fetchData, interval])

  return { data, error, loading, refetch: fetchData }
}

export async function apiGet<T>(url: string): Promise<T> {
  const res = await fetch(url, { credentials: 'include' })
  if (res.status === 401) {
    on401()
    throw new Error('unauthorized')
  }
  return res.json()
}

export async function apiPost<T>(url: string, body: unknown): Promise<T> {
  const res = await fetch(url, {
    method: 'POST',
    credentials: 'include',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  })
  if (res.status === 401) {
    on401()
    throw new Error('unauthorized')
  }
  return res.json()
}

export async function apiPut<T>(url: string, body: unknown): Promise<T> {
  const res = await fetch(url, {
    method: 'PUT',
    credentials: 'include',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  })
  if (res.status === 401) {
    on401()
    throw new Error('unauthorized')
  }
  return res.json()
}

export async function apiDelete<T>(url: string, body: unknown): Promise<T> {
  const res = await fetch(url, {
    method: 'DELETE',
    credentials: 'include',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  })
  if (res.status === 401) {
    on401()
    throw new Error('unauthorized')
  }
  return res.json()
}

/* Unauthenticated POST — for login and setup endpoints. credentials:
 * 'include' is still set so the server's Set-Cookie response (from
 * /api/auth/login) gets stored by the browser. */
export async function apiPostPublic<T>(url: string, body: unknown): Promise<T> {
  const res = await fetch(url, {
    method: 'POST',
    credentials: 'include',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  })
  return res.json()
}
