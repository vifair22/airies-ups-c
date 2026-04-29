import { screen, waitFor } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { describe, it, expect, vi, beforeEach } from 'vitest'
import { renderWithRouter, mockApiResponses } from '../test/helpers'
import WeatherConfig from './WeatherConfig'
import type { WeatherStatus, WeatherConfigData } from '../types/weather'
import type { ConfigReg } from '../types/config'

beforeEach(() => {
  vi.restoreAllMocks()
  localStorage.setItem('auth_token', 'test-jwt')
})

const enabledStatus: WeatherStatus = {
  enabled: true, severe: false,
}

const severeStatus: WeatherStatus = {
  enabled: true, severe: true, simulated: false,
  reasons: 'Tornado Warning in effect',
}

const simulatedStatus: WeatherStatus = {
  enabled: true, severe: true, simulated: true,
  reasons: 'High winds, severe thunderstorm',
}

const disabledStatus: WeatherStatus = {
  enabled: false,
}

const mockConfig: WeatherConfigData = {
  enabled: true, latitude: 35.0, longitude: -85.0,
  alert_zones: 'TNC155', alert_types: 'Tornado Warning\nSevere Thunderstorm Warning',
  wind_speed_mph: 50, severe_keywords: 'tornado,hail',
  poll_interval: 300, control_register: 'ups_sensitivity',
}

const mockRegs: ConfigReg[] = [
  {
    name: 'ups_sensitivity', display_name: 'UPS Sensitivity', type: 'bitfield',
    raw_value: 2, value: 2, writable: true, group: 'input',
    options: [
      { value: 0, name: 'low', label: 'Low' },
      { value: 2, name: 'normal', label: 'Normal' },
    ],
  },
  {
    name: 'battery_voltage', display_name: 'Battery Voltage', type: 'scalar',
    raw_value: 546, value: 54.6, writable: false, group: 'battery',
  },
]

function mockAll(statusOverride?: WeatherStatus) {
  mockApiResponses({
    '/api/weather/status': statusOverride ?? enabledStatus,
    '/api/weather/config': mockConfig,
    '/api/config/ups': mockRegs,
  })
}

/* ══════════════════════════════════════════════════════
 *  Status Display
 * ══════════════════════════════════════════════════════ */

describe('WeatherConfig — Status', () => {
  it('shows all-clear when enabled and not severe', async () => {
    mockAll()
    renderWithRouter(<WeatherConfig />)

    await waitFor(() => {
      expect(screen.getByText('Monitoring — All Clear')).toBeInTheDocument()
    })
  })

  it('shows disabled state', async () => {
    mockAll(disabledStatus)
    renderWithRouter(<WeatherConfig />)

    await waitFor(() => {
      expect(screen.getByText('Disabled')).toBeInTheDocument()
    })
  })

  it('shows severe weather active with reason', async () => {
    mockAll(severeStatus)
    renderWithRouter(<WeatherConfig />)

    await waitFor(() => {
      expect(screen.getByText('Severe Weather Active')).toBeInTheDocument()
    })
    expect(screen.getByText('Tornado Warning in effect')).toBeInTheDocument()
  })

  it('shows SIMULATED badge for simulated events', async () => {
    mockAll(simulatedStatus)
    renderWithRouter(<WeatherConfig />)

    await waitFor(() => {
      expect(screen.getByText('SIMULATED')).toBeInTheDocument()
    })
  })

  it('shows Clear Simulation button during severe weather', async () => {
    mockAll(severeStatus)
    renderWithRouter(<WeatherConfig />)

    await waitFor(() => {
      expect(screen.getByText('Clear Simulation')).toBeInTheDocument()
    })
  })

  it('does not show action buttons when disabled', async () => {
    mockAll(disabledStatus)
    renderWithRouter(<WeatherConfig />)

    await waitFor(() => {
      expect(screen.getByText('Disabled')).toBeInTheDocument()
    })
    expect(screen.queryByText('View Weather Report')).not.toBeInTheDocument()
    expect(screen.queryByText('Simulate Event')).not.toBeInTheDocument()
  })
})

/* ══════════════════════════════════════════════════════
 *  Action Buttons
 * ══════════════════════════════════════════════════════ */

describe('WeatherConfig — Actions', () => {
  it('shows report and simulate buttons when enabled', async () => {
    mockAll()
    renderWithRouter(<WeatherConfig />)

    await waitFor(() => {
      expect(screen.getByText('View Weather Report')).toBeInTheDocument()
    })
    expect(screen.getByText('Simulate Event')).toBeInTheDocument()
  })

  it('opens simulate modal', async () => {
    mockAll()
    renderWithRouter(<WeatherConfig />)

    await waitFor(() => {
      expect(screen.getByText('Simulate Event')).toBeInTheDocument()
    })

    await userEvent.click(screen.getByText('Simulate Event'))
    expect(screen.getByText('Simulate Severe Weather')).toBeInTheDocument()
    expect(screen.getByText('Trigger Severe')).toBeInTheDocument()
    expect(screen.getByDisplayValue('High winds, severe thunderstorm')).toBeInTheDocument()
  })

  it('opens weather report modal', async () => {
    /* Mock the report fetch separately since it's a direct fetch, not useApi */
    const mockReport = {
      alerts: [
        { event: 'Tornado Warning', headline: 'Tornado spotted', severity: 'Extreme', urgency: 'Immediate', matched: true },
      ],
      forecast: [
        { name: 'Tonight', temperature: 55, wind: '15 mph', wind_direction: 'SW', short_forecast: 'Storms likely', detailed_forecast: '' },
      ],
    }

    globalThis.fetch = vi.fn().mockImplementation((url: string) => {
      if (url.includes('/api/weather/report')) {
        return Promise.resolve({
          ok: true, status: 200,
          json: () => Promise.resolve(mockReport),
        })
      }
      /* Default responses for useApi hooks */
      if (url.includes('/api/weather/status')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(enabledStatus) })
      }
      if (url.includes('/api/weather/config')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(mockConfig) })
      }
      if (url.includes('/api/config/ups')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(mockRegs) })
      }
      return Promise.resolve({ ok: false, status: 404, json: () => Promise.resolve({}) })
    })

    renderWithRouter(<WeatherConfig />)

    await waitFor(() => {
      expect(screen.getByText('View Weather Report')).toBeInTheDocument()
    })

    await userEvent.click(screen.getByText('View Weather Report'))

    await waitFor(() => {
      expect(screen.getByText('NWS Weather Report')).toBeInTheDocument()
    })
    expect(screen.getByText('Tornado Warning')).toBeInTheDocument()
    expect(screen.getByText('MATCHED')).toBeInTheDocument()
    expect(screen.getByText('Tonight')).toBeInTheDocument()
    expect(screen.getByText('Storms likely')).toBeInTheDocument()
  })
})

/* ══════════════════════════════════════════════════════
 *  Config Form
 * ══════════════════════════════════════════════════════ */

describe('WeatherConfig — Config Form', () => {
  it('renders monitoring section with fields', async () => {
    mockAll()
    renderWithRouter(<WeatherConfig />)

    await waitFor(() => {
      expect(screen.getByText('Monitoring')).toBeInTheDocument()
    })
    expect(screen.getByText('Latitude')).toBeInTheDocument()
    expect(screen.getByText('Longitude')).toBeInTheDocument()
    expect(screen.getByText('NWS Alert Zones')).toBeInTheDocument()
    expect(screen.getByText('Wind Threshold (mph)')).toBeInTheDocument()
    expect(screen.getByText('Severe Keywords')).toBeInTheDocument()
    expect(screen.getByText('Poll Interval (seconds)')).toBeInTheDocument()
  })

  it('renders enable checkbox with correct state', async () => {
    mockAll()
    renderWithRouter(<WeatherConfig />)

    await waitFor(() => {
      expect(screen.getByLabelText('Enable weather monitoring')).toBeChecked()
    })
  })

  it('renders alert types textarea', async () => {
    mockAll()
    renderWithRouter(<WeatherConfig />)

    await waitFor(() => {
      expect(screen.getByText('Alert Types')).toBeInTheDocument()
    })
    expect(screen.getByDisplayValue(/Tornado Warning/)).toBeInTheDocument()
  })

  it('renders parameter override section', async () => {
    mockAll()
    renderWithRouter(<WeatherConfig />)

    await waitFor(() => {
      expect(screen.getByText('Parameter Override')).toBeInTheDocument()
    })
    expect(screen.getByText('Control Register')).toBeInTheDocument()
  })

  it('shows writable registers in control register dropdown', async () => {
    mockAll()
    renderWithRouter(<WeatherConfig />)

    await waitFor(() => {
      expect(screen.getByText('Parameter Override')).toBeInTheDocument()
    })

    /* Only the writable register should appear */
    const options = screen.getAllByRole('option')
    const regOptions = options.filter(o => o.textContent === 'UPS Sensitivity')
    expect(regOptions.length).toBeGreaterThanOrEqual(1)
  })

  it('shows bitfield options for severe/clear value when register is bitfield type', async () => {
    mockAll()
    renderWithRouter(<WeatherConfig />)

    await waitFor(() => {
      expect(screen.getByText('Severe Value')).toBeInTheDocument()
    })
    expect(screen.getByText('Clear Action')).toBeInTheDocument()
  })

  it('shows save button', async () => {
    mockAll()
    renderWithRouter(<WeatherConfig />)

    await waitFor(() => {
      expect(screen.getByRole('button', { name: 'Save' })).toBeInTheDocument()
    })
  })

  it('shows loading skeleton when UPS registers are not yet loaded', async () => {
    /* Return null for ups regs to show skeleton */
    globalThis.fetch = vi.fn().mockImplementation((url: string) => {
      if (url.includes('/api/weather/status')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(enabledStatus) })
      }
      if (url.includes('/api/weather/config')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(mockConfig) })
      }
      if (url.includes('/api/config/ups')) {
        /* Never resolve — simulates loading */
        return new Promise(() => {})
      }
      return Promise.resolve({ ok: false, status: 404, json: () => Promise.resolve({}) })
    })

    renderWithRouter(<WeatherConfig />)

    await waitFor(() => {
      expect(screen.getByText('Parameter Override')).toBeInTheDocument()
    })
    expect(document.querySelectorAll('.animate-pulse').length).toBeGreaterThan(0)
  })

  it('save button posts config to all setters', async () => {
    let savePosts = 0
    globalThis.fetch = vi.fn().mockImplementation((url: string, opts?: RequestInit) => {
      if (url.includes('/api/weather/config') && opts?.method === 'POST') {
        savePosts++
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve({ ok: true }) })
      }
      if (url.includes('/api/weather/status')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(enabledStatus) })
      }
      if (url.includes('/api/weather/config')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(mockConfig) })
      }
      if (url.includes('/api/config/ups')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(mockRegs) })
      }
      return Promise.resolve({ ok: false, status: 404, json: () => Promise.resolve({}) })
    })

    renderWithRouter(<WeatherConfig />)

    await waitFor(() => {
      expect(screen.getByRole('button', { name: 'Save' })).toBeInTheDocument()
    })

    await userEvent.click(screen.getByRole('button', { name: 'Save' }))

    await waitFor(() => {
      expect(savePosts).toBeGreaterThan(0)
    })
  })

  it('changing severe value selection updates form state', async () => {
    mockAll()
    const { container } = renderWithRouter(<WeatherConfig />)

    await waitFor(() => {
      expect(screen.getByText('Severe Value')).toBeInTheDocument()
    })

    /* The severe value select is the first select right after "Severe Value" label.
     * Its options are derived from mockRegs[0].options ([Low, Normal]). */
    const selects = container.querySelectorAll('select')
    const severeSelect = Array.from(selects).find(s =>
      Array.from(s.options).some(o => o.text === 'Low')
    ) as HTMLSelectElement | undefined
    expect(severeSelect).toBeTruthy()

    await userEvent.selectOptions(severeSelect!, '0')
    expect((severeSelect as HTMLSelectElement).value).toBe('0')
  })

  it('shows non-bitfield register controls when register is not bitfield type', async () => {
    const numericRegs = [
      { name: 'ups_transfer_high', display_name: 'Transfer High', type: 'number',
        raw_value: 127, value: 127, writable: true, group: 'input' },
    ]
    const configWithNumReg = { ...mockConfig, control_register: 'ups_transfer_high' }

    mockApiResponses({
      '/api/weather/status': enabledStatus,
      '/api/weather/config': configWithNumReg,
      '/api/config/ups': numericRegs,
    })

    renderWithRouter(<WeatherConfig />)

    await waitFor(() => {
      expect(screen.getByText('Severe Value (raw)')).toBeInTheDocument()
    })
    expect(screen.getByText('Clear Action')).toBeInTheDocument()
  })
})

/* ══════════════════════════════════════════════════════
 *  Simulation Execution
 * ══════════════════════════════════════════════════════ */

describe('WeatherConfig — Simulation', () => {
  it('triggers simulation and shows toast', async () => {
    globalThis.fetch = vi.fn().mockImplementation((url: string, opts?: RequestInit) => {
      if (url.includes('/api/weather/simulate') && opts?.method === 'POST') {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve({ ok: true }) })
      }
      if (url.includes('/api/weather/status')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(enabledStatus) })
      }
      if (url.includes('/api/weather/config')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(mockConfig) })
      }
      if (url.includes('/api/config/ups')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(mockRegs) })
      }
      return Promise.resolve({ ok: false, status: 404, json: () => Promise.resolve({}) })
    })

    renderWithRouter(<WeatherConfig />)

    await waitFor(() => {
      expect(screen.getByText('Simulate Event')).toBeInTheDocument()
    })

    await userEvent.click(screen.getByText('Simulate Event'))
    expect(screen.getByText('Simulate Severe Weather')).toBeInTheDocument()

    await userEvent.click(screen.getByText('Trigger Severe'))

    await waitFor(() => {
      expect(screen.getByText(/Severe weather event injected/)).toBeInTheDocument()
    })
  })

  it('clears simulation and shows toast', async () => {
    globalThis.fetch = vi.fn().mockImplementation((url: string, opts?: RequestInit) => {
      if (url.includes('/api/weather/simulate') && opts?.method === 'POST') {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve({ ok: true }) })
      }
      if (url.includes('/api/weather/status')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(severeStatus) })
      }
      if (url.includes('/api/weather/config')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(mockConfig) })
      }
      if (url.includes('/api/config/ups')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(mockRegs) })
      }
      return Promise.resolve({ ok: false, status: 404, json: () => Promise.resolve({}) })
    })

    renderWithRouter(<WeatherConfig />)

    await waitFor(() => {
      expect(screen.getByText('Clear Simulation')).toBeInTheDocument()
    })

    await userEvent.click(screen.getByText('Clear Simulation'))

    await waitFor(() => {
      expect(screen.getByText(/Simulation cleared/)).toBeInTheDocument()
    })
  })

  it('report modal shows empty alert state', async () => {
    globalThis.fetch = vi.fn().mockImplementation((url: string) => {
      if (url.includes('/api/weather/report')) {
        return Promise.resolve({
          ok: true, status: 200,
          json: () => Promise.resolve({ alerts: [], forecast: [] }),
        })
      }
      if (url.includes('/api/weather/status')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(enabledStatus) })
      }
      if (url.includes('/api/weather/config')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(mockConfig) })
      }
      if (url.includes('/api/config/ups')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(mockRegs) })
      }
      return Promise.resolve({ ok: false, status: 404, json: () => Promise.resolve({}) })
    })

    renderWithRouter(<WeatherConfig />)

    await waitFor(() => {
      expect(screen.getByText('View Weather Report')).toBeInTheDocument()
    })

    await userEvent.click(screen.getByText('View Weather Report'))

    await waitFor(() => {
      expect(screen.getByText('No active alerts for configured zones.')).toBeInTheDocument()
    })
    expect(screen.getByText('No forecast data available.')).toBeInTheDocument()
  })
})
