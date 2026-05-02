import { render, screen, waitFor } from '@testing-library/react'
import { MemoryRouter } from 'react-router-dom'
import { describe, it, expect, vi, beforeEach } from 'vitest'
import App from './App'

beforeEach(() => {
  vi.restoreAllMocks()
})

function renderApp(route = '/') {
  return render(
    <MemoryRouter initialEntries={[route]}>
      <App />
    </MemoryRouter>
  )
}

/* Helper: mock fetch to return different responses per URL pattern.
 * Auth is now decided by /api/auth/check (the only signal the JS layer
 * gets — HttpOnly cookie is unreadable). */
type Resp = { ok: boolean; status: number; jsonBody?: unknown }

function mockFetchByUrl(map: Array<[RegExp | string, Resp]>) {
  globalThis.fetch = vi.fn().mockImplementation((url: string) => {
    for (const [match, resp] of map) {
      const hit = match instanceof RegExp ? match.test(url) : url.includes(match)
      if (hit) {
        return Promise.resolve({
          ok: resp.ok,
          status: resp.status,
          json: () => Promise.resolve(resp.jsonBody ?? {}),
        })
      }
    }
    return Promise.resolve({
      ok: true, status: 200, json: () => Promise.resolve({}),
    })
  })
}

describe('AuthGuard / App routing', () => {
  it('redirects to /setup when setup is needed', async () => {
    mockFetchByUrl([
      ['setup/status', { ok: true, status: 200, jsonBody: { needs_setup: true } }],
    ])
    renderApp('/')
    await waitFor(() => {
      expect(screen.getByText('Initial Setup')).toBeInTheDocument()
    })
  })

  it('redirects to /login when auth check fails', async () => {
    mockFetchByUrl([
      ['setup/status', { ok: true, status: 200, jsonBody: { needs_setup: false } }],
      ['auth/check',   { ok: false, status: 401 }],
    ])
    renderApp('/')
    await waitFor(() => {
      expect(screen.getByText('Admin Password')).toBeInTheDocument()
    })
  })

  it('renders protected layout when authenticated', async () => {
    mockFetchByUrl([
      ['setup/status', { ok: true, status: 200, jsonBody: { needs_setup: false } }],
      ['auth/check',   { ok: true,  status: 200, jsonBody: { ok: true } }],
      [/.*/, { ok: true, status: 200, jsonBody: {
        driver: 'modbus', connected: true, name: 'TestUPS',
      } }],
    ])
    renderApp('/')
    await waitFor(() => {
      expect(screen.getByText('Dashboard')).toBeInTheDocument()
    })
  })

  it('renders login page at /login without auth check', () => {
    renderApp('/login')
    expect(screen.getByRole('button', { name: 'Login' })).toBeInTheDocument()
  })

  it('shows loading spinner during auth check', () => {
    globalThis.fetch = vi.fn().mockReturnValue(new Promise(() => {}))
    renderApp('/')
    expect(document.querySelector('.animate-spin')).toBeInTheDocument()
  })
})
