import { screen, waitFor } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { describe, it, expect, vi, beforeEach } from 'vitest'
import { renderWithRouter, mockApiResponses } from '../test/helpers'
import Events from './Events'

beforeEach(() => {
  vi.restoreAllMocks()
  localStorage.setItem('auth_token', 'test-jwt')
})

const sampleEvents = [
  { timestamp: '2026-04-17T10:00:00', severity: 'info', category: 'status', title: 'UPS Online', message: 'UPS is now online' },
  { timestamp: '2026-04-17T09:55:00', severity: 'warning', category: 'power', title: 'Input Fluctuation', message: 'Input voltage fluctuated briefly' },
  { timestamp: '2026-04-17T09:50:00', severity: 'error', category: 'fault', title: 'Battery Fault', message: 'Battery system error detected' },
]

describe('Events', () => {
  it('renders event list', async () => {
    mockApiResponses({ '/api/events': sampleEvents })
    renderWithRouter(<Events />)

    await waitFor(() => {
      expect(screen.getByText('UPS Online')).toBeInTheDocument()
    })
    expect(screen.getByText('Input Fluctuation')).toBeInTheDocument()
    expect(screen.getByText('Battery Fault')).toBeInTheDocument()
    expect(screen.getByText('3 entries')).toBeInTheDocument()
  })

  it('shows empty state when no events', async () => {
    mockApiResponses({ '/api/events': [] })
    renderWithRouter(<Events />)

    await waitFor(() => {
      expect(screen.getByText('No events yet')).toBeInTheDocument()
    })
  })

  it('filters by severity', async () => {
    mockApiResponses({ '/api/events': sampleEvents })
    renderWithRouter(<Events />)

    await waitFor(() => {
      expect(screen.getByText('UPS Online')).toBeInTheDocument()
    })

    /* Click the 'error' severity filter chip (button in filter bar) */
    const filterButtons = screen.getAllByRole('button')
    const errorFilter = filterButtons.find(b => b.textContent === 'error')!
    await userEvent.click(errorFilter)

    await waitFor(() => {
      expect(screen.queryByText('UPS Online')).not.toBeInTheDocument()
    })
    expect(screen.getByText('Battery Fault')).toBeInTheDocument()
    expect(screen.queryByText('Input Fluctuation')).not.toBeInTheDocument()
    expect(screen.getByText(/1 of 3/)).toBeInTheDocument()
  })

  it('clears filters', async () => {
    mockApiResponses({ '/api/events': sampleEvents })
    renderWithRouter(<Events />)

    await waitFor(() => {
      expect(screen.getByText('UPS Online')).toBeInTheDocument()
    })

    const filterButtons = screen.getAllByRole('button')
    const errorFilter = filterButtons.find(b => b.textContent === 'error')!
    await userEvent.click(errorFilter)

    await waitFor(() => {
      expect(screen.queryByText('UPS Online')).not.toBeInTheDocument()
    })

    await userEvent.click(screen.getByText('Clear filters'))
    expect(screen.getByText('UPS Online')).toBeInTheDocument()
    expect(screen.getByText('3 entries')).toBeInTheDocument()
  })

  it('shows loading skeleton while fetching', () => {
    globalThis.fetch = vi.fn().mockReturnValue(new Promise(() => {}))
    renderWithRouter(<Events />)
    expect(document.querySelectorAll('.animate-pulse').length).toBeGreaterThan(0)
  })
})
