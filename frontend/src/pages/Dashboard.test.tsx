import { screen, waitFor } from '@testing-library/react'
import { describe, it, expect, vi, beforeEach } from 'vitest'
import { renderWithRouter, mockApiResponses } from '../test/helpers'
import Dashboard from './Dashboard'
import type { UpsStatus } from '../types/ups'

beforeEach(() => {
  vi.restoreAllMocks()
})

const connectedStatus: UpsStatus = {
  driver: 'modbus',
  topology: 'online_double',
  connected: true,
  inventory: {
    model: 'Smart-UPS SRT 3000',
    sku: 'SRT3000XLT',
    serial: 'AS1234567890',
    firmware: '08.3',
    nominal_va: 3000,
    nominal_watts: 2700,
    sog_config: 0,
  },
  status: { raw: 2, text: 'online' },
  battery: { charge_pct: 100, voltage: 54.6, runtime_sec: 3600 },
  output: { voltage: 120.1, frequency: 60.0, current: 5.2, load_pct: 23, energy_wh: 450 },
  input: { voltage: 121.3, status: 0, transfer_high: 127, transfer_low: 106, warn_offset: 5 },
  capabilities: [],
}

describe('Dashboard', () => {
  it('shows loading skeleton before data arrives', () => {
    /* Never resolve fetch */
    globalThis.fetch = vi.fn().mockReturnValue(new Promise(() => {}))
    renderWithRouter(<Dashboard />)
    expect(document.querySelectorAll('.animate-pulse').length).toBeGreaterThan(0)
  })

  it('shows error state when API fails', async () => {
    globalThis.fetch = vi.fn().mockResolvedValue({
      ok: false,
      status: 500,
      statusText: 'Internal Server Error',
      json: () => Promise.resolve({}),
    })
    renderWithRouter(<Dashboard />)

    await waitFor(() => {
      expect(screen.getByText('Connection Lost')).toBeInTheDocument()
    })
  })

  it('shows disconnected state', async () => {
    mockApiResponses({
      '/api/status': { driver: 'modbus', connected: false, message: 'No device found' },
    })
    renderWithRouter(<Dashboard />)

    await waitFor(() => {
      expect(screen.getByText('No UPS Connected')).toBeInTheDocument()
    })
    expect(screen.getByText(/No device found/)).toBeInTheDocument()
  })

  it('renders connected UPS with SKU and battery info', async () => {
    mockApiResponses({ '/api/status': connectedStatus })
    renderWithRouter(<Dashboard />)

    await waitFor(() => {
      expect(screen.getByText('SRT3000XLT')).toBeInTheDocument()
    })
    expect(screen.getByText('AS1234567890')).toBeInTheDocument()
    expect(screen.getByText('1h 0m remaining')).toBeInTheDocument()
  })

  it('falls back to model when SKU is missing', async () => {
    const noSku: UpsStatus = {
      ...connectedStatus,
      inventory: { ...connectedStatus.inventory!, sku: undefined },
    }
    mockApiResponses({ '/api/status': noSku })
    renderWithRouter(<Dashboard />)

    await waitFor(() => {
      expect(screen.getByText('Smart-UPS SRT 3000')).toBeInTheDocument()
    })
  })

  it('renders output load percentage', async () => {
    mockApiResponses({ '/api/status': connectedStatus })
    renderWithRouter(<Dashboard />)

    await waitFor(() => {
      expect(screen.getByText('23.0')).toBeInTheDocument()
    })
  })

  it('renders on-battery status correctly', async () => {
    const onBattery = {
      ...connectedStatus,
      status: { raw: 4, text: 'on_battery' },
      transfer_reason: 'LowInputVoltage',
    }
    mockApiResponses({ '/api/status': onBattery })
    renderWithRouter(<Dashboard />)

    await waitFor(() => {
      /* "On Battery" appears in StatusDot and UPS Plane badge */
      const matches = screen.getAllByText('On Battery')
      expect(matches.length).toBeGreaterThanOrEqual(1)
    })
    /* Transfer reason should be humanized */
    expect(screen.getAllByText('Low Voltage').length).toBeGreaterThanOrEqual(1)
  })
})
