import { render, screen, waitFor } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { describe, it, expect, vi, beforeEach } from 'vitest'
import { MemoryRouter, Routes, Route } from 'react-router-dom'
import Layout from './Layout'
import { mockApiResponses } from '../test/helpers'

beforeEach(() => {
  vi.restoreAllMocks()
})

/* Layout uses <Outlet />; mount it inside Routes so React Router has a tree.
 * The dummy child page lets us assert the main slot rendered. */
function renderLayout({ route = '/' } = {}) {
  return render(
    <MemoryRouter initialEntries={[route]}>
      <Routes>
        <Route element={<Layout />}>
          <Route path="/" element={<div data-testid="page-content">home</div>} />
          <Route path="/about" element={<div data-testid="page-content">about</div>} />
        </Route>
      </Routes>
    </MemoryRouter>
  )
}

describe('Layout', () => {
  it('falls back to airies-ups when no status loaded', () => {
    /* /api/status pending forever — simulate cold start */
    globalThis.fetch = vi.fn().mockReturnValue(new Promise(() => {}))
    renderLayout()
    /* Both header and sidebar render the fallback name */
    expect(screen.getAllByText('airies-ups').length).toBeGreaterThan(0)
  })

  it('renders ups name and connected status when /api/status resolves', async () => {
    mockApiResponses({
      '/api/status': { name: 'Lab UPS', connected: true, driver: 'modbus' },
      '/api/version': { daemon: '1.0.1' },
    })
    renderLayout()

    await waitFor(() => {
      expect(screen.getAllByText('Lab UPS').length).toBeGreaterThan(0)
    })
    expect(screen.getByText(/modbus connected/)).toBeInTheDocument()
  })

  it('renders disconnected status when status.connected is false', async () => {
    mockApiResponses({
      '/api/status': { name: 'X', connected: false },
      '/api/version': { daemon: '1.0.1' },
    })
    renderLayout()

    await waitFor(() => {
      expect(screen.getByText('disconnected')).toBeInTheDocument()
    })
  })

  it('mobile drawer opens via hamburger and closes via overlay', async () => {
    mockApiResponses({
      '/api/status': { name: 'X', connected: true, driver: 'modbus' },
      '/api/version': { daemon: '1.0.1' },
    })
    const { container } = renderLayout()

    /* Sidebar starts off-screen via -translate-x-full on the aside */
    const aside = container.querySelector('aside')!
    expect(aside.className).toContain('-translate-x-full')

    /* Click the hamburger */
    await userEvent.click(screen.getByLabelText('Open navigation menu'))
    expect(aside.className).toContain('translate-x-0')

    /* Overlay appears; clicking it closes */
    const overlay = container.querySelector('[aria-hidden="true"].fixed.inset-0') as HTMLElement
    expect(overlay).toBeTruthy()
    await userEvent.click(overlay)
    expect(aside.className).toContain('-translate-x-full')
  })

  it('mobile drawer close button closes the drawer', async () => {
    mockApiResponses({
      '/api/status': { name: 'X' },
      '/api/version': { daemon: '1.0.1' },
    })
    const { container } = renderLayout()

    await userEvent.click(screen.getByLabelText('Open navigation menu'))
    const aside = container.querySelector('aside')!
    expect(aside.className).toContain('translate-x-0')

    await userEvent.click(screen.getByLabelText('Close navigation menu'))
    expect(aside.className).toContain('-translate-x-full')
  })

  it('renders nav links and config sub-section', () => {
    globalThis.fetch = vi.fn().mockReturnValue(new Promise(() => {}))
    renderLayout()
    expect(screen.getByText('Dashboard')).toBeInTheDocument()
    expect(screen.getByText('Events')).toBeInTheDocument()
    expect(screen.getByText('Commands')).toBeInTheDocument()
    expect(screen.getByText('About')).toBeInTheDocument()
    expect(screen.getByText('Configuration')).toBeInTheDocument()
    expect(screen.getByText('UPS Registers')).toBeInTheDocument()
    expect(screen.getByText('App Settings')).toBeInTheDocument()
    expect(screen.getByText('Shutdown')).toBeInTheDocument()
    expect(screen.getByText('Weather')).toBeInTheDocument()
  })

  it('logout calls api and clears the auth token', async () => {
    localStorage.setItem('auth_token', 'before')

    /* Tracks the logout POST. Don't redefine window.location — that
     * leaks across tests in the full suite. The redirect itself is
     * left unverified; we assert the side effects we can without
     * mutating global state. */
    let logoutCalled = false
    globalThis.fetch = vi.fn().mockImplementation((url: string) => {
      if (String(url).includes('/api/auth/logout')) {
        logoutCalled = true
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve({}) })
      }
      return new Promise(() => {})
    })

    /* jsdom throws on window.location.href assignment by default in some
     * versions. Wrap in try/catch via the click — we just need the
     * earlier side effects (apiPost + localStorage.removeItem) to fire
     * before the throw. */
    renderLayout()
    try {
      await userEvent.click(screen.getByText('Logout'))
    } catch { /* navigation throw is fine */ }

    await waitFor(() => {
      expect(logoutCalled).toBe(true)
    })
    expect(localStorage.getItem('auth_token')).toBeNull()
  })

  it('renders UI version in footer', () => {
    globalThis.fetch = vi.fn().mockReturnValue(new Promise(() => {}))
    renderLayout()
    /* __APP_VERSION__ is Vite-injected — vitest config defines it from
     * release_version. Format: "UI <version>" with optional daemon suffix. */
    expect(screen.getByText(/UI /)).toBeInTheDocument()
  })
})
