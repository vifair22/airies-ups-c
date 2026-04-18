import { renderHook, waitFor } from '@testing-library/react'
import { describe, it, expect, vi, beforeEach } from 'vitest'
import { useApi, apiPost, apiPut, apiDelete, apiPostPublic } from './useApi'

function mockFetch(status: number, body: unknown, headers?: Record<string, string>) {
  return vi.fn().mockResolvedValue({
    ok: status >= 200 && status < 300,
    status,
    statusText: status === 200 ? 'OK' : 'Error',
    json: () => Promise.resolve(body),
    headers: new Headers(headers),
  })
}

beforeEach(() => {
  vi.restoreAllMocks()
})

describe('useApi', () => {
  it('fetches data and returns it', async () => {
    const payload = { voltage: 120, load: 42 }
    globalThis.fetch = mockFetch(200, payload)

    const { result } = renderHook(() => useApi<typeof payload>('/api/status'))

    expect(result.current.loading).toBe(true)

    await waitFor(() => expect(result.current.loading).toBe(false))

    expect(result.current.data).toEqual(payload)
    expect(result.current.error).toBeNull()
  })

  it('sends auth header when token exists', async () => {
    localStorage.setItem('auth_token', 'test-jwt')
    globalThis.fetch = mockFetch(200, {})

    renderHook(() => useApi('/api/status'))

    await waitFor(() => {
      expect(globalThis.fetch).toHaveBeenCalledWith('/api/status', {
        headers: { Authorization: 'Bearer test-jwt' },
      })
    })
  })

  it('sends no auth header without token', async () => {
    globalThis.fetch = mockFetch(200, {})

    renderHook(() => useApi('/api/status'))

    await waitFor(() => {
      expect(globalThis.fetch).toHaveBeenCalledWith('/api/status', {
        headers: {},
      })
    })
  })

  it('sets error on non-ok response', async () => {
    globalThis.fetch = mockFetch(500, {})

    const { result } = renderHook(() => useApi('/api/status'))

    await waitFor(() => expect(result.current.loading).toBe(false))

    expect(result.current.data).toBeNull()
    expect(result.current.error).toBe('500 Error')
  })

  it('redirects to login on 401', async () => {
    localStorage.setItem('auth_token', 'expired-token')
    globalThis.fetch = mockFetch(401, {})

    // jsdom doesn't support navigation, so mock location
    const originalLocation = window.location
    Object.defineProperty(window, 'location', {
      writable: true,
      value: { ...originalLocation, href: '' },
    })

    renderHook(() => useApi('/api/status'))

    await waitFor(() => {
      expect(localStorage.getItem('auth_token')).toBeNull()
      expect(window.location.href).toBe('/login')
    })

    Object.defineProperty(window, 'location', {
      writable: true,
      value: originalLocation,
    })
  })

  it('refetch re-fetches data', async () => {
    let callCount = 0
    globalThis.fetch = vi.fn().mockImplementation(() => {
      callCount++
      return Promise.resolve({
        ok: true,
        status: 200,
        statusText: 'OK',
        json: () => Promise.resolve({ count: callCount }),
      })
    })

    const { result } = renderHook(() => useApi<{ count: number }>('/api/status'))

    await waitFor(() => expect(result.current.loading).toBe(false))
    expect(result.current.data).toEqual({ count: 1 })

    await result.current.refetch()

    await waitFor(() => expect(result.current.data).toEqual({ count: 2 }))
  })
})

describe('apiPost', () => {
  it('sends POST with auth and JSON body', async () => {
    const body = { username: 'admin', action: 'shutdown' }
    const response = { ok: true }
    globalThis.fetch = mockFetch(200, response)
    localStorage.setItem('auth_token', 'jwt-token')

    const result = await apiPost('/api/commands', body)

    expect(result).toEqual(response)
    expect(globalThis.fetch).toHaveBeenCalledWith('/api/commands', {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        Authorization: 'Bearer jwt-token',
      },
      body: JSON.stringify(body),
    })
  })
})

describe('apiPut', () => {
  it('sends PUT with auth and JSON body', async () => {
    const body = { id: 1, name: 'updated' }
    const response = { ok: true }
    globalThis.fetch = mockFetch(200, response)
    localStorage.setItem('auth_token', 'jwt-token')

    const result = await apiPut('/api/shutdown/groups', body)

    expect(result).toEqual(response)
    expect(globalThis.fetch).toHaveBeenCalledWith('/api/shutdown/groups', {
      method: 'PUT',
      headers: {
        'Content-Type': 'application/json',
        Authorization: 'Bearer jwt-token',
      },
      body: JSON.stringify(body),
    })
  })

  it('redirects to login on 401', async () => {
    localStorage.setItem('auth_token', 'expired')
    globalThis.fetch = mockFetch(401, {})

    const originalLocation = window.location
    Object.defineProperty(window, 'location', {
      writable: true,
      value: { ...originalLocation, href: '' },
    })

    await expect(apiPut('/api/test', {})).rejects.toThrow('unauthorized')
    expect(localStorage.getItem('auth_token')).toBeNull()
    expect(window.location.href).toBe('/login')

    Object.defineProperty(window, 'location', {
      writable: true,
      value: originalLocation,
    })
  })
})

describe('apiDelete', () => {
  it('sends DELETE with auth and JSON body', async () => {
    const body = { id: 5 }
    const response = { ok: true }
    globalThis.fetch = mockFetch(200, response)
    localStorage.setItem('auth_token', 'jwt-token')

    const result = await apiDelete('/api/shutdown/targets', body)

    expect(result).toEqual(response)
    expect(globalThis.fetch).toHaveBeenCalledWith('/api/shutdown/targets', {
      method: 'DELETE',
      headers: {
        'Content-Type': 'application/json',
        Authorization: 'Bearer jwt-token',
      },
      body: JSON.stringify(body),
    })
  })

  it('redirects to login on 401', async () => {
    localStorage.setItem('auth_token', 'expired')
    globalThis.fetch = mockFetch(401, {})

    const originalLocation = window.location
    Object.defineProperty(window, 'location', {
      writable: true,
      value: { ...originalLocation, href: '' },
    })

    await expect(apiDelete('/api/test', {})).rejects.toThrow('unauthorized')
    expect(localStorage.getItem('auth_token')).toBeNull()

    Object.defineProperty(window, 'location', {
      writable: true,
      value: originalLocation,
    })
  })
})

describe('apiPostPublic', () => {
  it('sends POST without auth header', async () => {
    const body = { password: 'secret' }
    globalThis.fetch = mockFetch(200, { token: 'new-jwt' })

    await apiPostPublic('/api/auth/login', body)

    expect(globalThis.fetch).toHaveBeenCalledWith('/api/auth/login', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    })
  })
})
