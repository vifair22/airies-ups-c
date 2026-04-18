import { render } from '@testing-library/react'
import { describe, it, expect } from 'vitest'
import { PowerFlowSRT, PowerFlowStandby } from './PowerFlow'
import { ST } from '../types/ups'
import type { PowerFlowProps } from '../types/ups'

/* ── Helpers ── */

const baseProps: PowerFlowProps = {
  statusRaw: ST.ONLINE,
  inputVoltage: 121.3,
  outputVoltage: 120.1,
  batteryCharge: 100,
  batteryVoltage: 54.6,
  batteryError: 0,
  loadPct: 23,
  efficiency: 120, /* raw efficiency byte, ~93.75% */
  outputFrequency: 60.0,
}

function props(overrides: Partial<PowerFlowProps> = {}): PowerFlowProps {
  return { ...baseProps, ...overrides }
}

/* Query helpers for SVG content */

function getAnimatedFlowLines(container: HTMLElement) {
  return container.querySelectorAll('polyline[class]')
}

function getFlowLineCount(container: HTMLElement) {
  /* Animated polylines = active flow lines (each FlowLine renders 2 polylines when active: bg + animated) */
  return getAnimatedFlowLines(container).length
}

function hasText(container: HTMLElement, text: string): boolean {
  const textEls = container.querySelectorAll('text')
  return Array.from(textEls).some(el => el.textContent?.includes(text))
}

function getBlockBorderColors(container: HTMLElement): string[] {
  const rects = container.querySelectorAll('rect')
  return Array.from(rects).map(r => r.getAttribute('stroke') || '')
}

function hasHETrackingLine(container: HTMLElement): boolean {
  /* The HE tracking line is a <line> with strokeDasharray="3 4" */
  const lines = container.querySelectorAll('line[stroke-dasharray="3 4"]')
  return lines.length > 0
}

/* ══════════════════════════════════════════════════════════════════
 *  PowerFlowSRT — Double Conversion
 * ══════════════════════════════════════════════════════════════════ */

describe('PowerFlowSRT', () => {

  /* ── Stage blocks always present ── */

  it('renders all 6 stage blocks', () => {
    const { container } = render(<PowerFlowSRT {...props()} />)
    expect(hasText(container, 'INPUT')).toBe(true)
    expect(hasText(container, 'RECTIFIER')).toBe(true)
    expect(hasText(container, 'DC BUS')).toBe(true)
    expect(hasText(container, 'INVERTER')).toBe(true)
    expect(hasText(container, 'OUTPUT')).toBe(true)
    expect(hasText(container, 'BATTERY')).toBe(true)
    expect(hasText(container, 'STATIC BYPASS')).toBe(true)
  })

  /* ── Online / Normal Operation ── */

  describe('online (normal)', () => {
    it('shows input and output voltages', () => {
      const { container } = render(<PowerFlowSRT {...props()} />)
      expect(hasText(container, '121.3')).toBe(true)
      expect(hasText(container, '120.1')).toBe(true)
    })

    it('shows battery charge at 100%', () => {
      const { container } = render(<PowerFlowSRT {...props()} />)
      expect(hasText(container, '100%')).toBe(true)
    })

    it('shows inverter frequency', () => {
      const { container } = render(<PowerFlowSRT {...props()} />)
      expect(hasText(container, '60.0')).toBe(true)
    })

    it('shows load percentage', () => {
      const { container } = render(<PowerFlowSRT {...props()} />)
      expect(hasText(container, '23% load')).toBe(true)
    })

    it('shows battery voltage', () => {
      const { container } = render(<PowerFlowSRT {...props()} />)
      expect(hasText(container, '54.6 VDC')).toBe(true)
    })

    it('shows efficiency badge', () => {
      const { container } = render(<PowerFlowSRT {...props()} />)
      expect(hasText(container, '94% eff')).toBe(true)
    })

    it('has active flow lines on main path', () => {
      const { container } = render(<PowerFlowSRT {...props()} />)
      /* Online with full battery: rectifier path active (4 flow lines: input→rect, rect→dc, dc→inv, inv→out) */
      const animated = getFlowLineCount(container)
      expect(animated).toBe(4)  /* 4 active segments, no battery, no bypass */
    })

    it('does not show HE tracking line', () => {
      const { container } = render(<PowerFlowSRT {...props()} />)
      expect(hasHETrackingLine(container)).toBe(false)
    })

    it('uses green borders on active blocks', () => {
      const { container } = render(<PowerFlowSRT {...props()} />)
      const colors = getBlockBorderColors(container)
      /* Input, Rectifier, DC Bus, Inverter, Output should be active green (#22c55e) */
      const greenCount = colors.filter(c => c === '#22c55e').length
      expect(greenCount).toBeGreaterThanOrEqual(5)
    })
  })

  /* ── Online, Battery Charging ── */

  describe('online, battery charging', () => {
    it('has battery flow line active (charging)', () => {
      const { container } = render(<PowerFlowSRT {...props({ batteryCharge: 72 })} />)
      /* Main path (4) + battery charging (1) = 5 active flow lines */
      expect(getFlowLineCount(container)).toBe(5)
    })

    it('shows battery charge percentage', () => {
      const { container } = render(<PowerFlowSRT {...props({ batteryCharge: 72 })} />)
      expect(hasText(container, '72%')).toBe(true)
    })

    it('battery charging flow is slow animated', () => {
      const { container } = render(<PowerFlowSRT {...props({ batteryCharge: 72 })} />)
      const slowLines = container.querySelectorAll('.animate-flow-slow')
      expect(slowLines.length).toBeGreaterThan(0)
    })
  })

  /* ── On Battery ── */

  describe('on battery', () => {
    const onBatProps = props({
      statusRaw: ST.ON_BATTERY,
      inputVoltage: 0,
      batteryCharge: 85,
    })

    it('shows -- for input voltage when power is lost', () => {
      const { container } = render(<PowerFlowSRT {...onBatProps} />)
      expect(hasText(container, '--')).toBe(true)
    })

    it('has battery discharging flow line (reverse animated)', () => {
      const { container } = render(<PowerFlowSRT {...onBatProps} />)
      const reverseLines = container.querySelectorAll('.animate-flow-reverse')
      expect(reverseLines.length).toBeGreaterThan(0)
    })

    it('has DC bus → inverter → output path active', () => {
      const { container } = render(<PowerFlowSRT {...onBatProps} />)
      /* On battery: dc→inv, inv→out, battery discharge = 3 active flow lines */
      expect(getFlowLineCount(container)).toBe(3)
    })

    it('bypass path is not active', () => {
      const { container } = render(<PowerFlowSRT {...onBatProps} />)
      /* No bypass animation classes */
      const bypassLabel = Array.from(container.querySelectorAll('text')).find(t =>
        t.textContent === 'STATIC BYPASS'
      )
      /* Bypass label should be dim (not bypass color) */
      expect(bypassLabel?.getAttribute('fill')).not.toBe('#eab308')
    })
  })

  /* ── Bypass (Commanded) ── */

  describe('bypass (commanded)', () => {
    const bypassProps = props({
      statusRaw: ST.ONLINE | ST.BYPASS | ST.COMMANDED,
    })

    it('has bypass path active', () => {
      const { container } = render(<PowerFlowSRT {...bypassProps} />)
      /* Bypass active: bypass (1) + battery not charging (full) = 1 active flow line */
      /* Actually: inputActive=true but rectifierActive=false (isBypass), so no main path */
      /* Only bypass flow line is active */
      expect(getFlowLineCount(container)).toBe(1)
    })

    it('bypass label uses yellow color for commanded bypass', () => {
      const { container } = render(<PowerFlowSRT {...bypassProps} />)
      const bypassLabel = Array.from(container.querySelectorAll('text')).find(t =>
        t.textContent === 'STATIC BYPASS'
      )
      /* Commanded bypass color = #eab308 (yellow) */
      expect(bypassLabel?.getAttribute('fill')).toBe('#eab308')
    })
  })

  /* ── Bypass (Forced / Uncommanded) ── */

  describe('bypass (forced)', () => {
    const forcedBypassProps = props({
      statusRaw: ST.ONLINE | ST.BYPASS,
    })

    it('bypass label uses orange color for forced bypass', () => {
      const { container } = render(<PowerFlowSRT {...forcedBypassProps} />)
      const bypassLabel = Array.from(container.querySelectorAll('text')).find(t =>
        t.textContent === 'STATIC BYPASS'
      )
      /* Forced bypass color = #f97316 (orange) */
      expect(bypassLabel?.getAttribute('fill')).toBe('#f97316')
    })
  })

  /* ── High Efficiency Mode ── */

  describe('HE mode', () => {
    const heProps = props({
      statusRaw: ST.HE_MODE,
    })

    it('bypass path is active', () => {
      const { container } = render(<PowerFlowSRT {...heProps} />)
      const animated = getFlowLineCount(container)
      expect(animated).toBeGreaterThanOrEqual(1)
    })

    it('inverter shows Tracking', () => {
      const { container } = render(<PowerFlowSRT {...heProps} />)
      expect(hasText(container, 'Tracking')).toBe(true)
    })

    it('shows HE tracking dotted line', () => {
      const { container } = render(<PowerFlowSRT {...heProps} />)
      expect(hasHETrackingLine(container)).toBe(true)
    })

    it('bypass label uses green (HE) color', () => {
      const { container } = render(<PowerFlowSRT {...heProps} />)
      const bypassLabel = Array.from(container.querySelectorAll('text')).find(t =>
        t.textContent === 'STATIC BYPASS'
      )
      /* HE color = #22c55e (same as active green) */
      expect(bypassLabel?.getAttribute('fill')).toBe('#22c55e')
    })

    it('rectifier/DC bus/inverter borders use HE standby color', () => {
      const { container } = render(<PowerFlowSRT {...heProps} />)
      const colors = getBlockBorderColors(container)
      /* heStandby = #166534 */
      const heStandbyCount = colors.filter(c => c === '#166534').length
      expect(heStandbyCount).toBeGreaterThanOrEqual(3) /* rect, dc, inv */
    })
  })

  /* ── Fault ── */

  describe('fault', () => {
    const faultProps = props({
      statusRaw: ST.ONLINE | ST.FAULT,
    })

    it('uses red fault color on active path borders', () => {
      const { container } = render(<PowerFlowSRT {...faultProps} />)
      const colors = getBlockBorderColors(container)
      const redCount = colors.filter(c => c === '#ef4444').length
      expect(redCount).toBeGreaterThanOrEqual(1)
    })

    it('flow lines use fault color', () => {
      const { container } = render(<PowerFlowSRT {...faultProps} />)
      const animated = getAnimatedFlowLines(container)
      const faultLines = Array.from(animated).filter(p =>
        p.getAttribute('stroke') === '#ef4444'
      )
      expect(faultLines.length).toBeGreaterThan(0)
    })
  })

  /* ── Output Off ── */

  describe('output off', () => {
    const offProps = props({
      statusRaw: ST.ONLINE | ST.OUTPUT_OFF,
    })

    it('output block uses dead border color', () => {
      const { container } = render(<PowerFlowSRT {...offProps} />)
      const colors = getBlockBorderColors(container)
      /* dead = fallback '#262626' in jsdom */
      expect(colors).toContain('#262626')
    })

    it('inverter has no value (not active)', () => {
      const { container } = render(<PowerFlowSRT {...offProps} />)
      /* Inverter should NOT show frequency when output is off */
      expect(hasText(container, '60.0')).toBe(false)
    })
  })

  /* ── Battery Fault ── */

  describe('battery fault', () => {
    const batFaultProps = props({
      batteryError: 1,
    })

    it('shows Missing instead of charge', () => {
      const { container } = render(<PowerFlowSRT {...batFaultProps} />)
      expect(hasText(container, 'Missing')).toBe(true)
    })

    it('hides battery voltage', () => {
      const { container } = render(<PowerFlowSRT {...batFaultProps} />)
      expect(hasText(container, '54.6 VDC')).toBe(false)
    })

    it('battery border uses fault color', () => {
      const { container } = render(<PowerFlowSRT {...batFaultProps} />)
      const colors = getBlockBorderColors(container)
      expect(colors).toContain('#ef4444')
    })
  })

  /* ── Edge Cases ── */

  describe('edge cases', () => {
    it('hides efficiency badge when efficiency is -1', () => {
      const { container } = render(<PowerFlowSRT {...props({ efficiency: -1 })} />)
      expect(hasText(container, 'eff')).toBe(false)
    })

    it('load badge uses red color above 80%', () => {
      const { container } = render(<PowerFlowSRT {...props({ loadPct: 85 })} />)
      const loadText = Array.from(container.querySelectorAll('text')).find(t =>
        t.textContent?.includes('% load')
      )
      expect(loadText?.getAttribute('fill')).toBe('#ef4444')
    })

    it('load badge uses yellow color above 60%', () => {
      const { container } = render(<PowerFlowSRT {...props({ loadPct: 65 })} />)
      const loadText = Array.from(container.querySelectorAll('text')).find(t =>
        t.textContent?.includes('% load')
      )
      expect(loadText?.getAttribute('fill')).toBe('#eab308')
    })

    it('low battery charge uses red text color', () => {
      const { container } = render(<PowerFlowSRT {...props({ batteryCharge: 20 })} />)
      const batText = Array.from(container.querySelectorAll('text')).find(t =>
        t.textContent?.includes('20%')
      )
      expect(batText?.getAttribute('fill')).toBe('#ef4444')
    })

    it('mid battery charge uses yellow text color', () => {
      const { container } = render(<PowerFlowSRT {...props({ batteryCharge: 50 })} />)
      const batText = Array.from(container.querySelectorAll('text')).find(t =>
        t.textContent?.includes('50%')
      )
      expect(batText?.getAttribute('fill')).toBe('#eab308')
    })
  })
})

/* ══════════════════════════════════════════════════════════════════
 *  PowerFlowStandby
 * ══════════════════════════════════════════════════════════════════ */

describe('PowerFlowStandby', () => {

  it('renders all 4 stage blocks', () => {
    const { container } = render(<PowerFlowStandby {...props()} />)
    expect(hasText(container, 'INPUT')).toBe(true)
    expect(hasText(container, 'TRANSFER')).toBe(true)
    expect(hasText(container, 'OUTPUT')).toBe(true)
    expect(hasText(container, 'BATTERY')).toBe(true)
  })

  /* ── Utility Path (Online) ── */

  describe('utility path', () => {
    it('transfer switch shows Utility', () => {
      const { container } = render(<PowerFlowStandby {...props()} />)
      expect(hasText(container, 'Utility')).toBe(true)
    })

    it('shows input and output voltages', () => {
      const { container } = render(<PowerFlowStandby {...props()} />)
      expect(hasText(container, '121.3')).toBe(true)
      expect(hasText(container, '120.1')).toBe(true)
    })

    it('has input → switch → output flow active', () => {
      const { container } = render(<PowerFlowStandby {...props()} />)
      /* Full battery: input→switch, switch→output = 2 active flow lines */
      expect(getFlowLineCount(container)).toBe(2)
    })

    it('shows battery voltage', () => {
      const { container } = render(<PowerFlowStandby {...props()} />)
      expect(hasText(container, '54.6 VDC')).toBe(true)
    })
  })

  /* ── Utility + Charging ── */

  describe('utility path, battery charging', () => {
    it('has battery charging flow active', () => {
      const { container } = render(<PowerFlowStandby {...props({ batteryCharge: 60 })} />)
      /* input→switch, switch→output, switch↔battery = 3 */
      expect(getFlowLineCount(container)).toBe(3)
    })

    it('battery charging is slow animated', () => {
      const { container } = render(<PowerFlowStandby {...props({ batteryCharge: 60 })} />)
      const slowLines = container.querySelectorAll('.animate-flow-slow')
      expect(slowLines.length).toBeGreaterThan(0)
    })
  })

  /* ── Battery Path ── */

  describe('battery path', () => {
    const batProps = props({
      statusRaw: ST.ON_BATTERY,
      inputVoltage: 0,
      batteryCharge: 85,
    })

    it('transfer switch shows Battery', () => {
      const { container } = render(<PowerFlowStandby {...batProps} />)
      expect(hasText(container, 'Battery')).toBe(true)
    })

    it('battery discharge is reverse animated', () => {
      const { container } = render(<PowerFlowStandby {...batProps} />)
      const reverseLines = container.querySelectorAll('.animate-flow-reverse')
      expect(reverseLines.length).toBeGreaterThan(0)
    })

    it('transfer switch text color is battery yellow', () => {
      const { container } = render(<PowerFlowStandby {...batProps} />)
      const switchText = Array.from(container.querySelectorAll('text')).find(t =>
        t.textContent === 'Battery'
      )
      expect(switchText?.getAttribute('fill')).toBe('#eab308')
    })
  })

  /* ── Fault ── */

  describe('fault', () => {
    it('uses red colors on active paths', () => {
      const { container } = render(<PowerFlowStandby {...props({
        statusRaw: ST.ONLINE | ST.FAULT,
      })} />)
      const animated = getAnimatedFlowLines(container)
      const faultLines = Array.from(animated).filter(p =>
        p.getAttribute('stroke') === '#ef4444'
      )
      expect(faultLines.length).toBeGreaterThan(0)
    })
  })

  /* ── Output Off ── */

  describe('output off', () => {
    it('output border uses dead color', () => {
      const { container } = render(<PowerFlowStandby {...props({
        statusRaw: ST.ONLINE | ST.OUTPUT_OFF,
      })} />)
      const colors = getBlockBorderColors(container)
      expect(colors).toContain('#262626')
    })

    it('no flow lines are active', () => {
      const { container } = render(<PowerFlowStandby {...props({
        statusRaw: ST.ONLINE | ST.OUTPUT_OFF,
      })} />)
      expect(getFlowLineCount(container)).toBe(0)
    })
  })

  /* ── Battery Fault ── */

  describe('battery fault', () => {
    it('shows Missing', () => {
      const { container } = render(<PowerFlowStandby {...props({ batteryError: 1 })} />)
      expect(hasText(container, 'Missing')).toBe(true)
    })

    it('hides battery voltage', () => {
      const { container } = render(<PowerFlowStandby {...props({ batteryError: 1 })} />)
      expect(hasText(container, 'VDC')).toBe(false)
    })

    it('battery border is fault red', () => {
      const { container } = render(<PowerFlowStandby {...props({ batteryError: 1 })} />)
      const colors = getBlockBorderColors(container)
      expect(colors).toContain('#ef4444')
    })
  })

  /* ── Edge Cases ── */

  describe('edge cases', () => {
    it('load badge uses red above 80%', () => {
      const { container } = render(<PowerFlowStandby {...props({ loadPct: 90 })} />)
      const loadText = Array.from(container.querySelectorAll('text')).find(t =>
        t.textContent?.includes('% load')
      )
      expect(loadText?.getAttribute('fill')).toBe('#ef4444')
    })

    it('shows -- for zero input voltage', () => {
      const { container } = render(<PowerFlowStandby {...props({
        statusRaw: ST.ON_BATTERY,
        inputVoltage: 0,
      })} />)
      expect(hasText(container, '--')).toBe(true)
    })
  })
})
