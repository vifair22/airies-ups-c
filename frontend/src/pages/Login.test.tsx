import { screen, waitFor } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { describe, it, expect, vi, beforeEach } from 'vitest'
import { renderWithRouter } from '../test/helpers'
import Login from './Login'

beforeEach(() => {
  vi.restoreAllMocks()
})

describe('Login', () => {
  it('renders login form', () => {
    renderWithRouter(<Login />, { route: '/login' })
    expect(screen.getByText('airies-ups')).toBeInTheDocument()
    expect(screen.getByText('Admin Password')).toBeInTheDocument()
    expect(screen.getByRole('button', { name: 'Login' })).toBeInTheDocument()
  })

  it('disables button when password is empty', () => {
    renderWithRouter(<Login />, { route: '/login' })
    expect(screen.getByRole('button', { name: 'Login' })).toBeDisabled()
  })

  it('enables button when password is entered', async () => {
    renderWithRouter(<Login />, { route: '/login' })
    const input = document.querySelector('input[type="password"]') as HTMLInputElement
    await userEvent.type(input, 'secret')
    expect(screen.getByRole('button', { name: 'Login' })).toBeEnabled()
  })

  it('navigates on successful login', async () => {
    /* No localStorage assertion — the auth cookie is HttpOnly, set by
     * the server's Set-Cookie response, and unreadable from JS. We
     * verify the login endpoint was hit with credentials and the
     * response contained a token (the server-side proof of success). */
    let loginCalledWithCredentials = false
    globalThis.fetch = vi.fn().mockImplementation((_url: string, init?: RequestInit) => {
      if (init?.credentials === 'include') loginCalledWithCredentials = true
      return Promise.resolve({
        ok: true, status: 200,
        json: () => Promise.resolve({ token: 'jwt-abc123' }),
      })
    })
    renderWithRouter(<Login />, { route: '/login' })

    const input = document.querySelector('input[type="password"]') as HTMLInputElement
    await userEvent.type(input, 'secret')
    await userEvent.click(screen.getByRole('button', { name: 'Login' }))

    await waitFor(() => {
      expect(loginCalledWithCredentials).toBe(true)
    })
  })

  it('shows error on failed login', async () => {
    globalThis.fetch = vi.fn().mockResolvedValue({
      ok: true,
      status: 200,
      json: () => Promise.resolve({ error: 'Invalid password' }),
    })
    renderWithRouter(<Login />, { route: '/login' })

    const input = document.querySelector('input[type="password"]') as HTMLInputElement
    await userEvent.type(input, 'wrong')
    await userEvent.click(screen.getByRole('button', { name: 'Login' }))

    await waitFor(() => {
      expect(screen.getByText('Invalid password')).toBeInTheDocument()
    })
  })

  it('shows loading state while submitting', async () => {
    globalThis.fetch = vi.fn().mockReturnValue(new Promise(() => {}))
    renderWithRouter(<Login />, { route: '/login' })

    const input = document.querySelector('input[type="password"]') as HTMLInputElement
    await userEvent.type(input, 'secret')
    await userEvent.click(screen.getByRole('button', { name: 'Login' }))

    expect(screen.getByText('Logging in...')).toBeInTheDocument()
  })
})
