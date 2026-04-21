import { screen, waitFor } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { describe, it, expect, vi, beforeEach } from 'vitest'
import { renderWithRouter, mockApiResponses } from '../test/helpers'
import AppConfig from './AppConfig'

beforeEach(() => {
  vi.restoreAllMocks()
  localStorage.setItem('auth_token', 'test-jwt')
})

const mockConfig = [
  { key: 'ups.device', value: '/dev/ttyUSB0', type: 'string', default_value: '/dev/ttyUSB0', description: 'Serial device path' },
  { key: 'ups.baud', value: '9600', type: 'int', default_value: '9600', description: 'Baud rate' },
  { key: 'pushover.token', value: 'abc123', type: 'string', default_value: '', description: 'Pushover API token' },
]

describe('AppConfig', () => {
  it('renders config entries grouped by prefix', async () => {
    mockApiResponses({ '/api/config/app': mockConfig })
    renderWithRouter(<AppConfig />)

    await waitFor(() => {
      expect(screen.getByText('device')).toBeInTheDocument()
    })
    expect(screen.getByText('baud')).toBeInTheDocument()
    expect(screen.getByText('token')).toBeInTheDocument()
  })

  it('shows theme selector', async () => {
    mockApiResponses({ '/api/config/app': mockConfig })
    renderWithRouter(<AppConfig />)

    await waitFor(() => {
      expect(screen.getByText('Theme')).toBeInTheDocument()
    })
  })

  it('shows password change form', async () => {
    mockApiResponses({ '/api/config/app': mockConfig })
    renderWithRouter(<AppConfig />)

    await waitFor(() => {
      expect(screen.getByText('Change Password')).toBeInTheDocument()
    })
  })

  it('shows modified indicator for non-default values', async () => {
    mockApiResponses({ '/api/config/app': mockConfig })
    renderWithRouter(<AppConfig />)

    await waitFor(() => {
      /* pushover.token has value 'abc123' which differs from default '' */
      expect(screen.getByText('modified')).toBeInTheDocument()
    })
  })

  it('shows Save button when config value is changed', async () => {
    mockApiResponses({ '/api/config/app': mockConfig })
    renderWithRouter(<AppConfig />)

    await waitFor(() => {
      expect(screen.getByText('device')).toBeInTheDocument()
    })

    /* Change the device value */
    const deviceInput = screen.getByDisplayValue('/dev/ttyUSB0')
    await userEvent.clear(deviceInput)
    await userEvent.type(deviceInput, '/dev/ttyUSB1')

    expect(screen.getByRole('button', { name: 'Save' })).toBeInTheDocument()
  })

  it('shows default value hint', async () => {
    mockApiResponses({ '/api/config/app': mockConfig })
    renderWithRouter(<AppConfig />)

    await waitFor(() => {
      expect(screen.getByText('default: /dev/ttyUSB0')).toBeInTheDocument()
    })
  })

  it('renders theme options', async () => {
    mockApiResponses({ '/api/config/app': mockConfig })
    renderWithRouter(<AppConfig />)

    await waitFor(() => {
      expect(screen.getByText('Theme')).toBeInTheDocument()
    })
    expect(screen.getByText('Auto')).toBeInTheDocument()
    expect(screen.getByText('Dark')).toBeInTheDocument()
    expect(screen.getByText('Light')).toBeInTheDocument()
  })

  it('password change validates minimum length', async () => {
    mockApiResponses({ '/api/config/app': mockConfig })
    renderWithRouter(<AppConfig />)

    await waitFor(() => {
      expect(screen.getByText('Change Password')).toBeInTheDocument()
    })

    const pwInputs = document.querySelectorAll('input[type="password"]')
    await userEvent.type(pwInputs[0], 'old')
    await userEvent.type(pwInputs[1], 'ab')
    await userEvent.type(pwInputs[2], 'ab')
    await userEvent.click(screen.getByText('Change Password'))

    await waitFor(() => {
      expect(screen.getByText('New password must be at least 4 characters')).toBeInTheDocument()
    })
  })

  it('password change validates confirmation match', async () => {
    mockApiResponses({ '/api/config/app': mockConfig })
    renderWithRouter(<AppConfig />)

    await waitFor(() => {
      expect(screen.getByText('Change Password')).toBeInTheDocument()
    })

    const pwInputs = document.querySelectorAll('input[type="password"]')
    await userEvent.type(pwInputs[0], 'oldpass')
    await userEvent.type(pwInputs[1], 'newpass1')
    await userEvent.type(pwInputs[2], 'newpass2')
    await userEvent.click(screen.getByText('Change Password'))

    await waitFor(() => {
      expect(screen.getByText('Passwords do not match')).toBeInTheDocument()
    })
  })

  it('shows success message on password change', async () => {
    globalThis.fetch = vi.fn().mockImplementation((url: string) => {
      if (url.includes('/api/auth/change')) {
        return Promise.resolve({
          ok: true, status: 200,
          json: () => Promise.resolve({ result: 'ok' }),
        })
      }
      return Promise.resolve({
        ok: true, status: 200,
        json: () => Promise.resolve(mockConfig),
      })
    })

    renderWithRouter(<AppConfig />)

    await waitFor(() => {
      expect(screen.getByText('Change Password')).toBeInTheDocument()
    })

    const pwInputs = document.querySelectorAll('input[type="password"]')
    await userEvent.type(pwInputs[0], 'oldpass')
    await userEvent.type(pwInputs[1], 'newpass1')
    await userEvent.type(pwInputs[2], 'newpass1')
    await userEvent.click(screen.getByText('Change Password'))

    await waitFor(() => {
      expect(screen.getByText('Password changed')).toBeInTheDocument()
    })
  })
})
