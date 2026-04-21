import { screen, waitFor } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { describe, it, expect, vi, beforeEach } from 'vitest'
import { renderWithRouter } from '../test/helpers'
import Setup from './Setup'

beforeEach(() => {
  vi.restoreAllMocks()
})

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

  it('shows login step when password is already set', async () => {
    globalThis.fetch = vi.fn().mockResolvedValue({
      ok: true,
      status: 200,
      json: () => Promise.resolve({
        needs_setup: true,
        password_set: true,
        ups_configured: false,
        ups_connected: false,
      }),
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
    expect(localStorage.getItem('auth_token')).toBe('new-jwt')
  })

  it('shows connection step with serial/USB options', async () => {
    localStorage.setItem('auth_token', 'test-jwt')
    let callCount = 0
    globalThis.fetch = vi.fn().mockImplementation(() => {
      callCount++
      if (callCount === 1) {
        return Promise.resolve({ ok: true, status: 200,
          json: () => Promise.resolve({ needs_setup: true, password_set: true, ups_configured: false, ups_connected: false }),
        })
      }
      /* ports */
      return Promise.resolve({ ok: true, status: 200,
        json: () => Promise.resolve({ serial: ['/dev/ttyUSB0', '/dev/ttyUSB1'], usb: [] }),
      })
    })

    renderWithRouter(<Setup />, { route: '/setup' })

    await waitFor(() => {
      expect(screen.getByText('UPS Connection')).toBeInTheDocument()
    })
    expect(screen.getByText('Test Connection')).toBeInTheDocument()
    expect(screen.getByDisplayValue('/dev/ttyUSB0')).toBeInTheDocument()
  })

  it('shows successful test connection result', async () => {
    localStorage.setItem('auth_token', 'test-jwt')
    let callCount = 0
    globalThis.fetch = vi.fn().mockImplementation(() => {
      callCount++
      if (callCount === 1) {
        return Promise.resolve({ ok: true, status: 200,
          json: () => Promise.resolve({ needs_setup: true, password_set: true, ups_configured: false, ups_connected: false }),
        })
      }
      if (callCount === 2) {
        return Promise.resolve({ ok: true, status: 200,
          json: () => Promise.resolve({ serial: ['/dev/ttyUSB0'], usb: [] }),
        })
      }
      /* test connection */
      return Promise.resolve({ ok: true, status: 200,
        json: () => Promise.resolve({
          result: 'ok', driver: 'modbus', topology: 'online_double',
          inventory: { model: 'SRT3000', serial: 'ABC123', firmware: '1.0', nominal_va: 3000, nominal_watts: 2700 },
        }),
      })
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
    localStorage.setItem('auth_token', 'test-jwt')
    let callCount = 0
    globalThis.fetch = vi.fn().mockImplementation(() => {
      callCount++
      if (callCount === 1) {
        return Promise.resolve({ ok: true, status: 200,
          json: () => Promise.resolve({ needs_setup: true, password_set: true, ups_configured: false, ups_connected: false }),
        })
      }
      if (callCount === 2) {
        return Promise.resolve({ ok: true, status: 200,
          json: () => Promise.resolve({ serial: ['/dev/ttyUSB0'], usb: [] }),
        })
      }
      /* test connection */
      return Promise.resolve({ ok: true, status: 200,
        json: () => Promise.resolve({
          result: 'ok', driver: 'modbus',
          inventory: { model: 'SRT3000', serial: 'ABC', firmware: '1.0', nominal_va: 3000, nominal_watts: 2700 },
        }),
      })
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
})
