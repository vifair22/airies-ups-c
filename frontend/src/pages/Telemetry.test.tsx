import { screen, waitFor } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { describe, it, expect, vi, beforeEach } from 'vitest'
import { renderWithRouter, mockApiResponses } from '../test/helpers'
import Telemetry from './Telemetry'

/* uPlot needs a Canvas API that jsdom only stubs — replace the chart with a
 * lightweight marker so assertions can count rendered cards without the
 * component crashing on new uPlot(). Chart rendering is verified manually. */
vi.mock('../components/UPlotChart', () => ({
  UPlotChart: () => <div data-testid="uplot-chart" />,
}))

beforeEach(() => {
  vi.restoreAllMocks()
})

const samplePoints = [
  { timestamp: '2026-04-24 10:00:00', charge_pct: 100, load_pct: 20,
    input_voltage: 120, output_voltage: 120, battery_voltage: 27.2,
    output_frequency: 60, output_current: 1.5, runtime_sec: 1800, efficiency: 128 },
  { timestamp: '2026-04-24 10:00:30', charge_pct: 99, load_pct: 22,
    input_voltage: 121, output_voltage: 120, battery_voltage: 27.1,
    output_frequency: 60, output_current: 1.6, runtime_sec: 1790, efficiency: 127 },
]

describe('Telemetry', () => {
  it('renders all preset buttons', async () => {
    mockApiResponses({ '/api/telemetry': samplePoints })
    renderWithRouter(<Telemetry />)

    await waitFor(() => {
      expect(screen.getByText('5m')).toBeInTheDocument()
    })
    /* Check a few span-representative presets — not exhaustively every one. */
    expect(screen.getByText('1h')).toBeInTheDocument()
    expect(screen.getByText('1d')).toBeInTheDocument()
    expect(screen.getByText('90d')).toBeInTheDocument()
  })

  it('renders one chart per metric when data is present', async () => {
    mockApiResponses({ '/api/telemetry': samplePoints })
    renderWithRouter(<Telemetry />)

    await waitFor(() => {
      expect(screen.getAllByTestId('uplot-chart')).toHaveLength(9)
    })
    /* Sanity check: at least one known metric header rendered. */
    expect(screen.getByText('Battery Charge')).toBeInTheDocument()
    expect(screen.getByText('Efficiency')).toBeInTheDocument()
  })

  it('shows the empty state when no telemetry is returned', async () => {
    mockApiResponses({ '/api/telemetry': [] })
    renderWithRouter(<Telemetry />)

    await waitFor(() => {
      expect(screen.getByText('No telemetry data')).toBeInTheDocument()
    })
    /* No chart cards should render in the empty state. */
    expect(screen.queryAllByTestId('uplot-chart')).toHaveLength(0)
  })

  it('clicking a preset updates the From input', async () => {
    mockApiResponses({ '/api/telemetry': samplePoints })
    renderWithRouter(<Telemetry />)

    await waitFor(() => {
      expect(screen.getByText('5m')).toBeInTheDocument()
    })

    const fromInput = document.querySelector('input[type="datetime-local"]') as HTMLInputElement
    const beforeValue = fromInput.value

    await userEvent.click(screen.getByText('1d'))

    /* The From input should now reflect a value ~24h before "now" — i.e.
     * different from the default 1h window the page starts with. */
    expect(fromInput.value).not.toBe(beforeValue)
  })

  it('renders the sample count when data is present', async () => {
    mockApiResponses({ '/api/telemetry': samplePoints })
    renderWithRouter(<Telemetry />)

    await waitFor(() => {
      expect(screen.getByText('2 samples')).toBeInTheDocument()
    })
  })
})
