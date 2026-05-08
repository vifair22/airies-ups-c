import { screen, waitFor } from '@testing-library/react'
import { describe, it, expect, vi, beforeEach } from 'vitest'
import { renderWithRouter, mockApiResponses } from '../test/helpers'
import About from './About'

beforeEach(() => {
  vi.restoreAllMocks()
})

const sampleAbout = {
  inventory: {
    model:         'Smart-UPS SRT 1500',
    serial:        'AS1234567890',
    firmware:      'UPS 04.1',
    nominal_va:    1500,
    nominal_watts: 1000,
    sog_config:    3,
  },
  registers: [
    {
      name: 'manufacture_date',
      display_name: 'Manufacture Date',
      group: 'identity',
      category: 'identity',
      type: 'scalar',
      raw_value: 8800,
      value: 8800,
      writable: false,
      unit: 'days since 2000-01-01',
      date: '2024-01-15',
    },
    {
      name: 'battery_date',
      display_name: 'Battery Install Date',
      group: 'identity',
      category: 'config',
      type: 'scalar',
      raw_value: 8800,
      value: 8800,
      writable: true,
      unit: 'days since 2000-01-01',
      date: '2024-01-15',
    },
    {
      name: 'transfer_high',
      display_name: 'Upper Acceptable Input Voltage',
      group: 'transfer',
      category: 'config',
      type: 'scalar',
      raw_value: 132,
      value: 132,
      writable: true,
      unit: 'V',
    },
    {
      name: 'output_voltage',
      display_name: 'Output Voltage',
      group: 'output',
      category: 'measurement',
      type: 'scalar',
      raw_value: 1220,
      value: 122.0,
      writable: false,
      unit: 'V',
    },
    {
      name: 'ups_status',
      display_name: 'UPS Status',
      group: 'status',
      category: 'diagnostic',
      type: 'flags',
      raw_value: 0x00002002,
      value: 8194,
      writable: false,
      active_flags: [
        { value: 0x00000002, name: 'online',  label: 'Online' },
        { value: 0x00002000, name: 'he_mode', label: 'High Efficiency Mode' },
      ],
    },
    {
      name: 'bypass_voltage',
      display_name: 'Bypass Voltage',
      group: 'bypass',
      category: 'measurement',
      type: 'scalar',
      raw_value: 65535,
      value: 65535,
      writable: false,
      is_sentinel: true,
      unit: 'V',
    },
  ],
}

describe('About', () => {
  it('renders inventory and groups registers by category', async () => {
    mockApiResponses({ '/api/about': sampleAbout })
    renderWithRouter(<About />)

    await waitFor(() => {
      expect(screen.getByText('Smart-UPS SRT 1500')).toBeInTheDocument()
    })

    /* Category section headers should appear in order */
    expect(screen.getByText('Identity')).toBeInTheDocument()
    expect(screen.getByText('Measurements')).toBeInTheDocument()
    expect(screen.getByText('Configuration')).toBeInTheDocument()
    expect(screen.getByText('Diagnostics')).toBeInTheDocument()

    /* Field rows render with their decoded values */
    expect(screen.getByText('Manufacture Date')).toBeInTheDocument()
    expect(screen.getByText('Battery Install Date')).toBeInTheDocument()
    expect(screen.getByText('Upper Acceptable Input Voltage')).toBeInTheDocument()
    expect(screen.getByText('132 V')).toBeInTheDocument()
    expect(screen.getByText('122 V')).toBeInTheDocument()
  })

  it('renders flags type as comma-joined list of active flag labels', async () => {
    mockApiResponses({ '/api/about': sampleAbout })
    renderWithRouter(<About />)

    await waitFor(() => {
      expect(screen.getByText('UPS Status')).toBeInTheDocument()
    })
    expect(screen.getByText('Online, High Efficiency Mode')).toBeInTheDocument()
    /* 32-bit flags raw value renders as hex */
    expect(screen.getByText('0x00002002')).toBeInTheDocument()
  })

  it('renders is_sentinel as N/A and visually subdues the row', async () => {
    mockApiResponses({ '/api/about': sampleAbout })
    renderWithRouter(<About />)

    await waitFor(() => {
      expect(screen.getByText('Bypass Voltage')).toBeInTheDocument()
    })
    expect(screen.getByText('N/A')).toBeInTheDocument()
  })

  it('omits empty inventory fields', async () => {
    mockApiResponses({
      '/api/about': {
        inventory: {
          model: 'Test Model',
          serial: '',
          firmware: '',
          nominal_va: 1000,
          nominal_watts: 0,
          sog_config: 0,
        },
        registers: [],
      },
    })
    renderWithRouter(<About />)

    await waitFor(() => {
      expect(screen.getByText('Test Model')).toBeInTheDocument()
    })
    expect(screen.queryByText('Serial Number')).not.toBeInTheDocument()
    expect(screen.queryByText('Firmware')).not.toBeInTheDocument()
    expect(screen.queryByText('Nominal Watts')).not.toBeInTheDocument()
    expect(screen.queryByText('SOG Configuration')).not.toBeInTheDocument()
  })

  it('shows empty state when no device info available', async () => {
    mockApiResponses({ '/api/about': { registers: [] } })
    renderWithRouter(<About />)

    await waitFor(() => {
      expect(screen.getByText(/No device information available/)).toBeInTheDocument()
    })
  })
})
