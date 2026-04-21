import { describe, it, expect } from 'vitest'
import { fmtRuntime, fmtWatts, fmtVA, humanizeTransfer } from './format'

describe('fmtRuntime', () => {
  it('formats seconds as minutes only when under an hour', () => {
    expect(fmtRuntime(0)).toBe('0m')
    expect(fmtRuntime(59)).toBe('0m')
    expect(fmtRuntime(60)).toBe('1m')
    expect(fmtRuntime(300)).toBe('5m')
    expect(fmtRuntime(3599)).toBe('59m')
  })

  it('formats seconds as hours + minutes when over an hour', () => {
    expect(fmtRuntime(3600)).toBe('1h 0m')
    expect(fmtRuntime(3660)).toBe('1h 1m')
    expect(fmtRuntime(7200)).toBe('2h 0m')
    expect(fmtRuntime(7380)).toBe('2h 3m')
    expect(fmtRuntime(86400)).toBe('24h 0m')
  })
})

describe('fmtWatts', () => {
  it('formats small loads in watts', () => {
    expect(fmtWatts(10, 1000)).toBe('100 W')
    expect(fmtWatts(50, 500)).toBe('250 W')
    expect(fmtWatts(1, 100)).toBe('1 W')
  })

  it('formats large loads in kilowatts', () => {
    expect(fmtWatts(100, 1000)).toBe('1.00 kW')
    expect(fmtWatts(50, 3000)).toBe('1.50 kW')
    expect(fmtWatts(100, 2400)).toBe('2.40 kW')
  })

  it('handles zero load', () => {
    expect(fmtWatts(0, 1000)).toBe('0 W')
  })
})

describe('fmtVA', () => {
  it('formats small apparent power in VA', () => {
    expect(fmtVA(10, 1500)).toBe('150 VA')
    expect(fmtVA(50, 800)).toBe('400 VA')
  })

  it('formats large apparent power in kVA', () => {
    expect(fmtVA(100, 1500)).toBe('1.50 kVA')
    expect(fmtVA(100, 3000)).toBe('3.00 kVA')
  })

  it('handles zero load', () => {
    expect(fmtVA(0, 1500)).toBe('0 VA')
  })
})

describe('humanizeTransfer', () => {
  it('returns -- for undefined/empty', () => {
    expect(humanizeTransfer()).toBe('--')
    expect(humanizeTransfer(undefined)).toBe('--')
  })

  it('maps known transfer reasons', () => {
    expect(humanizeTransfer('HighInputVoltage')).toBe('High Voltage')
    expect(humanizeTransfer('LowInputVoltage')).toBe('Low Voltage')
    expect(humanizeTransfer('SystemInitialization')).toBe('System Init')
    expect(humanizeTransfer('AcceptableInput')).toBe('Input Acceptable')
    expect(humanizeTransfer('LocalUICommand')).toBe('Front Panel')
    expect(humanizeTransfer('ProtocolCommand')).toBe('Protocol Command')
    expect(humanizeTransfer('LowBatteryVoltage')).toBe('Low Battery')
    expect(humanizeTransfer('AutomaticRestart')).toBe('Auto Restart')
  })

  it('passes through unknown reasons verbatim', () => {
    expect(humanizeTransfer('SomeFutureReason')).toBe('SomeFutureReason')
    expect(humanizeTransfer('custom_value')).toBe('custom_value')
  })
})
