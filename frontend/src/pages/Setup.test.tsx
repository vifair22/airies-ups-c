import { screen, waitFor } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { describe, it, expect, vi, beforeEach } from 'vitest'
import { renderWithRouter } from '../test/helpers'
import Setup from './Setup'

beforeEach(() => {
  vi.restoreAllMocks()
})

/* URL-routed mock with FIFO fallback queue. Routes match by substring;
 * if no route matches, the next item from `fallback` is returned (so
 * existing tests that use call-order semantics keep working). All
 * responses default to ok:true unless overridden. */
type MockResp = { ok?: boolean; status?: number; jsonBody?: unknown }
function setFetch(routes: Record<string, MockResp>, fallback: MockResp[] = []) {
  let fbIdx = 0
  globalThis.fetch = vi.fn().mockImplementation((url: string) => {
    for (const [pat, r] of Object.entries(routes)) {
      if (url.includes(pat)) {
        return Promise.resolve({
          ok: r.ok ?? true, status: r.status ?? 200,
          json: () => Promise.resolve(r.jsonBody ?? {}),
        })
      }
    }
    const r = fallback[fbIdx] ?? fallback[fallback.length - 1] ?? { ok: true, status: 200 }
    fbIdx++
    return Promise.resolve({
      ok: r.ok ?? true, status: r.status ?? 200,
      json: () => Promise.resolve(r.jsonBody ?? {}),
    })
  })
}

describe('Setup', () => {
  it('shows loading spinner initially', () => {
    /* setup/status never resolves */
    globalThis.fetch = vi.fn().mockReturnValue(new Promise(() => {}))
    renderWithRouter(<Setup />, { route: '/setup' })
    expect(document.querySelector('.animate-spin')).toBeInTheDocument()
  })

  it('shows password step when setup is needed', async () => {
    globalThis.fetch = vi.fn().mockResolvedValue({
      ok: true,
      status: 200,
      json: () => Promise.resolve({
        needs_setup: true,
        password_set: false,
        ups_configured: false,
        ups_connected: false,
      }),
    })

    renderWithRouter(<Setup />, { route: '/setup' })

    await waitFor(() => {
      expect(screen.getByText('Set Admin Password')).toBeInTheDocument()
    })
    expect(screen.getByText(/Step 1 of 4/)).toBeInTheDocument()
  })

  it('shows login step when password is already set but not authed', async () => {
    globalThis.fetch = vi.fn().mockImplementation((url: string) => {
      if (url.includes('/api/auth/check')) {
        /* Cookie absent / invalid → unauthed → show login step */
        return Promise.resolve({ ok: false, status: 401, json: () => Promise.resolve({}) })
      }
      return Promise.resolve({
        ok: true, status: 200,
        json: () => Promise.resolve({
          needs_setup: true,
          password_set: true,
          ups_configured: false,
          ups_connected: false,
        }),
      })
    })

    renderWithRouter(<Setup />, { route: '/setup' })

    await waitFor(() => {
      expect(screen.getByText('Welcome Back')).toBeInTheDocument()
    })
  })

  it('validates password length', async () => {
    globalThis.fetch = vi.fn().mockResolvedValue({
      ok: true,
      status: 200,
      json: () => Promise.resolve({
        needs_setup: true,
        password_set: false,
        ups_configured: false,
        ups_connected: false,
      }),
    })

    renderWithRouter(<Setup />, { route: '/setup' })

    await waitFor(() => {
      expect(screen.getByText('Set Admin Password')).toBeInTheDocument()
    })

    const inputs = document.querySelectorAll('input[type="password"]')
    await userEvent.type(inputs[0], 'ab')
    await userEvent.type(inputs[1], 'ab')
    await userEvent.click(screen.getByRole('button', { name: 'Continue' }))

    await waitFor(() => {
      expect(screen.getByText('Password must be at least 4 characters')).toBeInTheDocument()
    })
  })

  it('advances to connection step after password set', async () => {
    let callCount = 0
    globalThis.fetch = vi.fn().mockImplementation(() => {
      callCount++
      if (callCount === 1) {
        /* setup/status — needs setup */
        return Promise.resolve({ ok: true, status: 200,
          json: () => Promise.resolve({ needs_setup: true, password_set: false, ups_configured: false, ups_connected: false }),
        })
      }
      if (callCount === 2) {
        /* auth/setup — password set successfully */
        return Promise.resolve({ ok: true, status: 200,
          json: () => Promise.resolve({ result: 'ok' }),
        })
      }
      if (callCount === 3) {
        /* auth/login — auto-login after password set */
        return Promise.resolve({ ok: true, status: 200,
          json: () => Promise.resolve({ token: 'new-jwt' }),
        })
      }
      /* setup/ports — port scan for connection step */
      return Promise.resolve({ ok: true, status: 200,
        json: () => Promise.resolve({ serial: ['/dev/ttyUSB0'], usb: [] }),
      })
    })

    renderWithRouter(<Setup />, { route: '/setup' })

    await waitFor(() => {
      expect(screen.getByText('Set Admin Password')).toBeInTheDocument()
    })

    const inputs = document.querySelectorAll('input[type="password"]')
    await userEvent.type(inputs[0], 'goodpass')
    await userEvent.type(inputs[1], 'goodpass')
    await userEvent.click(screen.getByRole('button', { name: 'Continue' }))

    await waitFor(() => {
      expect(screen.getByText('UPS Connection')).toBeInTheDocument()
    })
    /* Auth credential is now an HttpOnly cookie set by the server's
     * /api/auth/login response — unreadable from JS, so no client-side
     * assertion. The fact that we successfully advanced to the
     * connection step is the proof. */
  })

  it('shows connection step with serial/USB options', async () => {
    setFetch({
      '/api/setup/status': { jsonBody: { needs_setup: true, password_set: true, ups_configured: false, ups_connected: false } },
      '/api/auth/check':   { jsonBody: { ok: true } },
      '/api/setup/ports':  { jsonBody: { serial: ['/dev/ttyUSB0', '/dev/ttyUSB1'], usb: [] } },
    })

    renderWithRouter(<Setup />, { route: '/setup' })

    await waitFor(() => {
      expect(screen.getByText('UPS Connection')).toBeInTheDocument()
    })
    expect(screen.getByText('Test Connection')).toBeInTheDocument()
    expect(screen.getByDisplayValue('/dev/ttyUSB0')).toBeInTheDocument()
  })

  it('shows successful test connection result', async () => {
    setFetch({
      '/api/setup/status': { jsonBody: { needs_setup: true, password_set: true, ups_configured: false, ups_connected: false } },
      '/api/auth/check':   { jsonBody: { ok: true } },
      '/api/setup/ports':  { jsonBody: { serial: ['/dev/ttyUSB0'], usb: [] } },
      '/api/setup/test':   { jsonBody: {
        result: 'ok', driver: 'modbus', topology: 'online_double',
        inventory: { model: 'SRT3000', serial: 'ABC123', firmware: '1.0', nominal_va: 3000, nominal_watts: 2700 },
      } },
    })

    renderWithRouter(<Setup />, { route: '/setup' })

    await waitFor(() => {
      expect(screen.getByText('Test Connection')).toBeInTheDocument()
    })

    await userEvent.click(screen.getByText('Test Connection'))

    await waitFor(() => {
      expect(screen.getByText(/Connected — MODBUS driver/)).toBeInTheDocument()
    })
    expect(screen.getByText(/SRT3000/)).toBeInTheDocument()
  })

  it('shows notifications step', async () => {
    setFetch({
      '/api/setup/status': { jsonBody: { needs_setup: true, password_set: true, ups_configured: false, ups_connected: false } },
      '/api/auth/check':   { jsonBody: { ok: true } },
      '/api/setup/ports':  { jsonBody: { serial: ['/dev/ttyUSB0'], usb: [] } },
      '/api/setup/test':   { jsonBody: {
        result: 'ok', driver: 'modbus',
        inventory: { model: 'SRT3000', serial: 'ABC', firmware: '1.0', nominal_va: 3000, nominal_watts: 2700 },
      } },
    })

    renderWithRouter(<Setup />, { route: '/setup' })

    await waitFor(() => {
      expect(screen.getByText('Test Connection')).toBeInTheDocument()
    })

    await userEvent.click(screen.getByText('Test Connection'))
    await waitFor(() => {
      expect(screen.getByText(/Connected/)).toBeInTheDocument()
    })

    /* Continue button should now be enabled */
    const continueBtn = screen.getAllByRole('button').find(b => b.textContent === 'Continue')!
    await userEvent.click(continueBtn)

    await waitFor(() => {
      expect(screen.getByText('Notifications')).toBeInTheDocument()
    })
    expect(screen.getByText('Pushover API Token')).toBeInTheDocument()
  })

  it('validates password confirmation match', async () => {
    globalThis.fetch = vi.fn().mockResolvedValue({
      ok: true,
      status: 200,
      json: () => Promise.resolve({
        needs_setup: true,
        password_set: false,
        ups_configured: false,
        ups_connected: false,
      }),
    })

    renderWithRouter(<Setup />, { route: '/setup' })

    await waitFor(() => {
      expect(screen.getByText('Set Admin Password')).toBeInTheDocument()
    })

    const inputs = document.querySelectorAll('input[type="password"]')
    await userEvent.type(inputs[0], 'goodpass')
    await userEvent.type(inputs[1], 'different')
    await userEvent.click(screen.getByRole('button', { name: 'Continue' }))

    await waitFor(() => {
      expect(screen.getByText('Passwords do not match')).toBeInTheDocument()
    })
  })

  it('login step submits and advances to connection', async () => {
    setFetch({
      '/api/setup/status': { jsonBody: { needs_setup: true, password_set: true, ups_configured: false, ups_connected: false } },
      /* auth/check returns 401 the first time so AuthGuard puts us on
       * the login step. After login, the same endpoint isn't called
       * again here — the user moves forward via state, not by re-probing. */
      '/api/auth/check':   { ok: false, status: 401 },
      '/api/auth/login':   { jsonBody: { token: 'login-jwt' } },
      '/api/setup/ports':  { jsonBody: { serial: ['/dev/ttyUSB0'], usb: [] } },
    })

    renderWithRouter(<Setup />, { route: '/setup' })

    await waitFor(() => {
      expect(screen.getByText('Welcome Back')).toBeInTheDocument()
    })

    await userEvent.type(document.querySelector('input[type="password"]')!, 'goodpass')
    await userEvent.click(screen.getByRole('button', { name: 'Continue' }))

    await waitFor(() => {
      expect(screen.getByText('UPS Connection')).toBeInTheDocument()
    })
    /* No localStorage assertion — the auth cookie is HttpOnly. */
  })

  it('login error from server is surfaced', async () => {
    setFetch({
      '/api/setup/status': { jsonBody: { needs_setup: true, password_set: true, ups_configured: false, ups_connected: false } },
      '/api/auth/check':   { ok: false, status: 401 },
      '/api/auth/login':   { jsonBody: { error: 'invalid password' } },
    })

    renderWithRouter(<Setup />, { route: '/setup' })

    await waitFor(() => {
      expect(screen.getByText('Welcome Back')).toBeInTheDocument()
    })

    await userEvent.type(document.querySelector('input[type="password"]')!, 'wrong')
    await userEvent.click(screen.getByRole('button', { name: 'Continue' }))

    await waitFor(() => {
      expect(screen.getByText('invalid password')).toBeInTheDocument()
    })
  })

  it('connection step switches to USB form when user selects USB', async () => {
    setFetch({
      '/api/setup/status': { jsonBody: { needs_setup: true, password_set: true, ups_configured: false, ups_connected: false } },
      '/api/auth/check':   { jsonBody: { ok: true } },
      /* Mixed port scan — serial wins auto-select; user manually picks USB */
      '/api/setup/ports':  { jsonBody: {
        serial: ['/dev/ttyUSB0'],
        usb: [{ vid: '051d', pid: '0002', name: 'APC Back-UPS', device: '/dev/hidraw0' }],
      } },
    })

    renderWithRouter(<Setup />, { route: '/setup' })

    await waitFor(() => {
      expect(screen.getByText('UPS Connection')).toBeInTheDocument()
    })

    /* Switch to USB via the Connection Type dropdown */
    const connTypeSelect = screen.getByDisplayValue('Serial (Modbus RTU)') as HTMLSelectElement
    await userEvent.selectOptions(connTypeSelect, 'usb')

    await waitFor(() => {
      expect(screen.getByText('Detected USB Devices')).toBeInTheDocument()
    })
  })

  it('test connection error is shown', async () => {
    setFetch({
      '/api/setup/status': { jsonBody: { needs_setup: true, password_set: true, ups_configured: false, ups_connected: false } },
      '/api/auth/check':   { jsonBody: { ok: true } },
      '/api/setup/ports':  { jsonBody: { serial: ['/dev/ttyUSB0'], usb: [] } },
      '/api/setup/test':   { jsonBody: { error: 'no UPS detected on /dev/ttyUSB0' } },
    })

    renderWithRouter(<Setup />, { route: '/setup' })

    await waitFor(() => {
      expect(screen.getByText('Test Connection')).toBeInTheDocument()
    })

    await userEvent.click(screen.getByText('Test Connection'))

    await waitFor(() => {
      expect(screen.getByText(/no UPS detected/)).toBeInTheDocument()
    })
  })

  it('save config with pushover and restart polls back to dashboard', async () => {
    let restarted = false
    let postRestartStatus = 0  /* simulate daemon down then up */
    globalThis.fetch = vi.fn().mockImplementation((url: string, opts?: RequestInit) => {
      const u = String(url)
      if (u.includes('/api/setup/status')) {
        if (restarted) {
          postRestartStatus++
          if (postRestartStatus < 2) {
            return Promise.reject(new Error('not ready'))
          }
          return Promise.resolve({ ok: true, status: 200,
            json: () => Promise.resolve({ needs_setup: false, password_set: true, ups_configured: true, ups_connected: true }),
          })
        }
        return Promise.resolve({ ok: true, status: 200,
          json: () => Promise.resolve({ needs_setup: true, password_set: true, ups_configured: false, ups_connected: false }),
        })
      }
      if (u.includes('/api/auth/check')) {
        return Promise.resolve({ ok: true, status: 200,
          json: () => Promise.resolve({ ok: true }),
        })
      }
      if (u.includes('/api/setup/ports')) {
        return Promise.resolve({ ok: true, status: 200,
          json: () => Promise.resolve({ serial: ['/dev/ttyUSB0'], usb: [] }),
        })
      }
      if (u.includes('/api/setup/test')) {
        return Promise.resolve({ ok: true, status: 200,
          json: () => Promise.resolve({
            result: 'ok', driver: 'modbus',
            inventory: { model: 'SRT3000', serial: 'A', firmware: '1', nominal_va: 3000, nominal_watts: 2700 },
          }),
        })
      }
      if (u.includes('/api/config/app') && opts?.method === 'POST') {
        return Promise.resolve({ ok: true, status: 200,
          json: () => Promise.resolve({ result: 'ok' }),
        })
      }
      if (u.includes('/api/restart')) {
        restarted = true
        return Promise.resolve({ ok: true, status: 200,
          json: () => Promise.resolve({ result: 'ok' }),
        })
      }
      return Promise.resolve({ ok: false, status: 404, json: () => Promise.resolve({}) })
    })

    renderWithRouter(<Setup />, { route: '/setup' })

    await waitFor(() => {
      expect(screen.getByText('Test Connection')).toBeInTheDocument()
    })

    /* Fill in UPS Name (covers the if(upsName) setConfig branch) */
    const upsNameInput = screen.getByPlaceholderText(/Server Room/)
    await userEvent.type(upsNameInput, 'Lab')

    await userEvent.click(screen.getByText('Test Connection'))
    await waitFor(() => expect(screen.getByText(/Connected/)).toBeInTheDocument())

    await userEvent.click(
      screen.getAllByRole('button').find(b => b.textContent === 'Continue')!
    )

    await waitFor(() => expect(screen.getByText('Notifications')).toBeInTheDocument())

    /* Fill in pushover credentials → exercises the if(pushToken && pushUser) branch */
    const inputs = screen.getAllByPlaceholderText('Leave blank to skip')
    await userEvent.type(inputs[0], 'token-abc')
    await userEvent.type(inputs[1], 'user-def')

    /* Button text becomes "Save & Finish" when pushToken is set */
    await userEvent.click(screen.getByText('Save & Finish'))

    await waitFor(() => expect(screen.getByText('Setup Complete')).toBeInTheDocument())

    /* Restart click triggers /api/restart and the polling loop */
    await userEvent.click(screen.getByText('Restart & Go to Dashboard'))
    await waitFor(() => expect(screen.getByText('Restarting...')).toBeInTheDocument())
  })

  it('save config from notifications step lands on done step', async () => {
    globalThis.fetch = vi.fn().mockImplementation((url: string, opts?: RequestInit) => {
      const u = String(url)
      if (u.includes('/api/setup/status')) {
        return Promise.resolve({ ok: true, status: 200,
          json: () => Promise.resolve({ needs_setup: true, password_set: true, ups_configured: false, ups_connected: false }),
        })
      }
      if (u.includes('/api/auth/check')) {
        return Promise.resolve({ ok: true, status: 200,
          json: () => Promise.resolve({ ok: true }),
        })
      }
      if (u.includes('/api/setup/ports')) {
        return Promise.resolve({ ok: true, status: 200,
          json: () => Promise.resolve({ serial: ['/dev/ttyUSB0'], usb: [] }),
        })
      }
      if (u.includes('/api/setup/test')) {
        return Promise.resolve({ ok: true, status: 200,
          json: () => Promise.resolve({
            result: 'ok', driver: 'modbus',
            inventory: { model: 'SRT3000', serial: 'A', firmware: '1', nominal_va: 3000, nominal_watts: 2700 },
          }),
        })
      }
      if (u.includes('/api/config/app') && opts?.method === 'POST') {
        return Promise.resolve({ ok: true, status: 200,
          json: () => Promise.resolve({ result: 'ok' }),
        })
      }
      return Promise.resolve({ ok: false, status: 404, json: () => Promise.resolve({}) })
    })

    renderWithRouter(<Setup />, { route: '/setup' })

    await waitFor(() => {
      expect(screen.getByText('Test Connection')).toBeInTheDocument()
    })

    await userEvent.click(screen.getByText('Test Connection'))
    await waitFor(() => {
      expect(screen.getByText(/Connected/)).toBeInTheDocument()
    })

    /* connection → notifications */
    await userEvent.click(
      screen.getAllByRole('button').find(b => b.textContent === 'Continue')!
    )

    await waitFor(() => {
      expect(screen.getByText('Notifications')).toBeInTheDocument()
    })

    /* Skip pushover (don't fill in token/user). Click "Skip" or "Save". */
    const buttons = screen.getAllByRole('button')
    const finishBtn = buttons.find(b =>
      b.textContent === 'Skip & Finish' || b.textContent === 'Finish' ||
      b.textContent === 'Save' || b.textContent === 'Save & Finish'
    )
    expect(finishBtn).toBeDefined()
    await userEvent.click(finishBtn!)

    /* saveConfig posts each config key — at least one POST hit */
    await waitFor(() => {
      const calls = (globalThis.fetch as ReturnType<typeof vi.fn>).mock.calls
      const configPosts = calls.filter((call: unknown[]) => {
        const [url, optsAny] = call as [string, RequestInit?]
        return url.includes('/api/config/app') && optsAny?.method === 'POST'
      })
      expect(configPosts.length).toBeGreaterThan(0)
    })
  })
})
