import { screen, waitFor } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { describe, it, expect, vi, beforeEach } from 'vitest'
import { renderWithRouter, mockApiResponses } from '../test/helpers'
import UpsConfig from './UpsConfig'

beforeEach(() => {
  vi.restoreAllMocks()
  localStorage.setItem('auth_token', 'test-jwt')
})

const mockRegs = [
  {
    name: 'ups_sensitivity',
    display_name: 'UPS Sensitivity',
    group: 'input',
    type: 'bitfield',
    raw_value: 2,
    value: 2,
    writable: true,
    setting: 'normal',
    setting_label: 'Normal',
    options: [
      { value: 0, name: 'low', label: 'Low' },
      { value: 1, name: 'medium', label: 'Medium' },
      { value: 2, name: 'normal', label: 'Normal' },
    ],
  },
  {
    name: 'battery_voltage',
    display_name: 'Battery Voltage',
    group: 'battery',
    type: 'number',
    raw_value: 546,
    value: 54.6,
    writable: false,
    unit: 'VDC',
  },
]

describe('UpsConfig', () => {
  it('shows loading skeleton while fetching', () => {
    globalThis.fetch = vi.fn().mockReturnValue(new Promise(() => {}))
    renderWithRouter(<UpsConfig />)
    expect(document.querySelectorAll('.animate-pulse').length).toBeGreaterThan(0)
  })

  it('renders register groups and values', async () => {
    mockApiResponses({ '/api/config/ups': mockRegs })
    renderWithRouter(<UpsConfig />)

    await waitFor(() => {
      expect(screen.getByText('UPS Sensitivity')).toBeInTheDocument()
    })
    expect(screen.getByText('Normal')).toBeInTheDocument()
    expect(screen.getByText('Battery Voltage')).toBeInTheDocument()
    expect(screen.getByText('54.6 VDC')).toBeInTheDocument()
  })

  it('shows edit button for writable registers', async () => {
    mockApiResponses({ '/api/config/ups': mockRegs })
    renderWithRouter(<UpsConfig />)

    await waitFor(() => {
      expect(screen.getByText('Edit')).toBeInTheDocument()
    })
  })

  it('shows read-only label for non-writable registers', async () => {
    mockApiResponses({ '/api/config/ups': mockRegs })
    renderWithRouter(<UpsConfig />)

    await waitFor(() => {
      expect(screen.getByText('read-only')).toBeInTheDocument()
    })
  })

  it('opens inline editor on Edit click', async () => {
    mockApiResponses({ '/api/config/ups': mockRegs })
    renderWithRouter(<UpsConfig />)

    await waitFor(() => {
      expect(screen.getByText('Edit')).toBeInTheDocument()
    })

    await userEvent.click(screen.getByText('Edit'))
    expect(screen.getByText('Save')).toBeInTheDocument()
    expect(screen.getByText('Cancel')).toBeInTheDocument()
  })

  it('shows empty state when no registers', async () => {
    mockApiResponses({ '/api/config/ups': [] })
    renderWithRouter(<UpsConfig />)

    await waitFor(() => {
      expect(screen.getByText(/Registers unavailable/)).toBeInTheDocument()
    })
  })

  it('cancels inline editor', async () => {
    mockApiResponses({ '/api/config/ups': mockRegs })
    renderWithRouter(<UpsConfig />)

    await waitFor(() => {
      expect(screen.getByText('Edit')).toBeInTheDocument()
    })

    await userEvent.click(screen.getByText('Edit'))
    expect(screen.getByText('Save')).toBeInTheDocument()

    await userEvent.click(screen.getByText('Cancel'))
    /* Should return to display mode */
    expect(screen.getByText('Edit')).toBeInTheDocument()
    expect(screen.queryByText('Save')).not.toBeInTheDocument()
  })

  it('saves register value and shows feedback', async () => {
    globalThis.fetch = vi.fn().mockImplementation((url: string, opts?: RequestInit) => {
      if (opts?.method === 'POST' && url.includes('/api/config/ups')) {
        return Promise.resolve({
          ok: true, status: 200,
          json: () => Promise.resolve({ result: 'written' }),
        })
      }
      return Promise.resolve({
        ok: true, status: 200,
        json: () => Promise.resolve(mockRegs),
      })
    })

    renderWithRouter(<UpsConfig />)

    await waitFor(() => {
      expect(screen.getByText('Edit')).toBeInTheDocument()
    })

    await userEvent.click(screen.getByText('Edit'))

    /* For bitfield type, there's a select dropdown */
    const select = screen.getByRole('combobox')
    await userEvent.selectOptions(select, '0')

    await userEvent.click(screen.getByText('Save'))

    await waitFor(() => {
      expect(screen.getByText('saved')).toBeInTheDocument()
    })
  })

  it('shows register raw value', async () => {
    mockApiResponses({ '/api/config/ups': mockRegs })
    renderWithRouter(<UpsConfig />)

    await waitFor(() => {
      expect(screen.getByText('2')).toBeInTheDocument() /* raw_value of sensitivity */
    })
    expect(screen.getByText('546')).toBeInTheDocument() /* raw_value of battery voltage */
  })

  it('shows group headers', async () => {
    mockApiResponses({ '/api/config/ups': mockRegs })
    renderWithRouter(<UpsConfig />)

    await waitFor(() => {
      expect(screen.getByText('input')).toBeInTheDocument()
    })
    expect(screen.getByText('battery')).toBeInTheDocument()
  })

  it('expands a register row to show change history', async () => {
    /* History-route key comes first so url.includes() matches it before the
     * more general '/api/config/ups' entry — helpers use insertion order. */
    mockApiResponses({
      '/api/config/ups/history': [
        { timestamp: '2026-04-20 09:00:00', raw_value: 110, display_value: '110 V', source: 'external' },
        { timestamp: '2026-04-15 12:00:00', raw_value: 103, display_value: '103 V', source: 'api' },
        { timestamp: '2026-04-01 00:00:00', raw_value: 103, display_value: '103 V', source: 'baseline' },
      ],
      '/api/config/ups': mockRegs,
    })
    renderWithRouter(<UpsConfig />)

    await waitFor(() => {
      expect(screen.getByText('UPS Sensitivity')).toBeInTheDocument()
    })

    /* Click anywhere on the display portion of the row (not the Action cell) */
    await userEvent.click(screen.getByText('UPS Sensitivity'))

    await waitFor(() => {
      expect(screen.getByText('Change history')).toBeInTheDocument()
    })
    expect(screen.getByText('2026-04-20 09:00:00')).toBeInTheDocument()
    expect(screen.getByText('LCD / external')).toBeInTheDocument()
    expect(screen.getByText('UI')).toBeInTheDocument()
    expect(screen.getByText('baseline')).toBeInTheDocument()
  })

  it('starting an edit does not collapse history or toggle expansion', async () => {
    mockApiResponses({
      '/api/config/ups/history': [],
      '/api/config/ups': mockRegs,
    })
    renderWithRouter(<UpsConfig />)

    await waitFor(() => {
      expect(screen.getByText('Edit')).toBeInTheDocument()
    })

    /* Clicking the Edit button inside the Action cell must not trigger the
     * row-level expansion handler (marked data-no-toggle). */
    await userEvent.click(screen.getByText('Edit'))
    expect(screen.getByText('Save')).toBeInTheDocument()
    expect(screen.queryByText('Change history')).not.toBeInTheDocument()
  })
})
