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

describe('AuthGuard / App routing', () => {
  it('redirects to /setup when setup is needed', async () => {
    globalThis.fetch = vi.fn().mockResolvedValue({
      ok: true,
      status: 200,
      json: () => Promise.resolve({ needs_setup: true }),
    })

    renderApp('/')

    await waitFor(() => {
      expect(screen.getByText('Initial Setup')).toBeInTheDocument()
    })
  })

  it('redirects to /login when no token exists', async () => {
    globalThis.fetch = vi.fn().mockResolvedValue({
      ok: true,
      status: 200,
      json: () => Promise.resolve({ needs_setup: false }),
    })

    renderApp('/')

    await waitFor(() => {
      expect(screen.getByText('Admin Password')).toBeInTheDocument()
    })
  })

  it('renders protected layout when authenticated', async () => {
    localStorage.setItem('auth_token', 'valid-jwt')

    /* setup/status returns no setup needed, then /api/status for Layout */
    globalThis.fetch = vi.fn().mockImplementation((url: string) => {
      if (url.includes('setup/status')) {
        return Promise.resolve({
          ok: true, status: 200,
          json: () => Promise.resolve({ needs_setup: false }),
        })
      }
      return Promise.resolve({
        ok: true, status: 200,
        json: () => Promise.resolve({ driver: 'modbus', connected: true, name: 'TestUPS' }),
      })
    })

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
