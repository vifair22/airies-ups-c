/* Dev harness for PowerFlow diagrams.
 * Renders all topology variants with live-adjustable props.
 * Only accessible in dev mode at /dev/powerflow. */

import { useState } from 'react'
import { PowerFlowSRT, PowerFlowLineInteractive, PowerFlowStandby } from '../components/PowerFlow'
import { ST } from '../types/ups'
import type { PowerFlowProps } from '../types/ups'

/* ── Status bit toggles ── */

const STATUS_BITS = [
  { key: 'ONLINE',         bit: ST.ONLINE,         label: 'Online' },
  { key: 'ON_BATTERY',     bit: ST.ON_BATTERY,     label: 'On Battery' },
  { key: 'BYPASS',         bit: ST.BYPASS,          label: 'Bypass' },
  { key: 'OUTPUT_OFF',     bit: ST.OUTPUT_OFF,      label: 'Output Off' },
  { key: 'FAULT',          bit: ST.FAULT,           label: 'Fault' },
  { key: 'INPUT_BAD',      bit: ST.INPUT_BAD,       label: 'Input Bad' },
  { key: 'TEST',           bit: ST.TEST,            label: 'Self-Test' },
  { key: 'PENDING_ON',     bit: ST.PENDING_ON,      label: 'Pending On' },
  { key: 'SHUT_PENDING',   bit: ST.SHUT_PENDING,    label: 'Shut Pending' },
  { key: 'COMMANDED',      bit: ST.COMMANDED,       label: 'Commanded' },
  { key: 'HE_MODE',        bit: ST.HE_MODE,         label: 'HE' },
  { key: 'AVR_BOOST',      bit: ST.AVR_BOOST,       label: 'AVR Boost' },
  { key: 'AVR_TRIM',       bit: ST.AVR_TRIM,        label: 'AVR Trim' },
  { key: 'FAULT_STATE',    bit: ST.FAULT_STATE,     label: 'Fault State' },
  { key: 'MAINS_BAD',      bit: ST.MAINS_BAD,       label: 'Mains Bad' },
  { key: 'FAULT_RECOVERY', bit: ST.FAULT_RECOVERY,  label: 'Fault Recovery' },
  { key: 'OVERLOAD',       bit: ST.OVERLOAD,        label: 'Overload' },
] as const

/* ── Presets for common states ── */

interface Preset {
  label: string
  statusRaw: number
  overrides?: Partial<PowerFlowProps>
}

const PRESETS: Preset[] = [
  { label: 'Online (normal)', statusRaw: ST.ONLINE },
  { label: 'Online (charging)', statusRaw: ST.ONLINE, overrides: { batteryCharge: 72 } },
  { label: 'On Battery', statusRaw: ST.ON_BATTERY, overrides: { inputVoltage: 0, batteryCharge: 85 } },
  { label: 'On Battery (low)', statusRaw: ST.ON_BATTERY, overrides: { inputVoltage: 0, batteryCharge: 18 } },
  { label: 'Bypass (commanded)', statusRaw: ST.ONLINE | ST.BYPASS | ST.COMMANDED },
  { label: 'Bypass (forced)', statusRaw: ST.ONLINE | ST.BYPASS },
  { label: 'HE Mode', statusRaw: ST.ONLINE | ST.HE_MODE },
  { label: 'AVR Boost', statusRaw: ST.ONLINE | ST.AVR_BOOST, overrides: { inputVoltage: 104.2 } },
  { label: 'AVR Trim', statusRaw: ST.ONLINE | ST.AVR_TRIM, overrides: { inputVoltage: 128.5 } },
  { label: 'Fault', statusRaw: ST.ONLINE | ST.FAULT },
  { label: 'Fault + On Battery', statusRaw: ST.ON_BATTERY | ST.FAULT, overrides: { inputVoltage: 0 } },
  { label: 'Output Off', statusRaw: ST.ONLINE | ST.OUTPUT_OFF },
  { label: 'Battery Missing', statusRaw: ST.ONLINE, overrides: { batteryError: 1 } },
  { label: 'Overload', statusRaw: ST.ONLINE | ST.OVERLOAD, overrides: { loadPct: 95 } },
  { label: 'Everything off', statusRaw: 0, overrides: { inputVoltage: 0, outputVoltage: 0, batteryCharge: 0 } },
]

/* ── Slider control ── */

function Slider({ label, value, onChange, min, max, step = 1, unit }: {
  label: string; value: number; onChange: (v: number) => void
  min: number; max: number; step?: number; unit?: string
}) {
  return (
    <div className="flex items-center gap-3">
      <label className="text-xs text-muted w-32 shrink-0">{label}</label>
      <input type="range" min={min} max={max} step={step} value={value}
        onChange={e => onChange(parseFloat(e.target.value))}
        className="flex-1 accent-accent h-1.5" />
      <span className="text-xs font-mono text-primary w-20 text-right">
        {value.toFixed(step < 1 ? 1 : 0)}{unit && <span className="text-faint ml-0.5">{unit}</span>}
      </span>
    </div>
  )
}

/* ── Main harness ── */

export default function DevPowerFlow() {
  const [statusRaw, setStatusRaw] = useState(ST.ONLINE)
  const [inputVoltage, setInputVoltage] = useState(121.3)
  const [outputVoltage, setOutputVoltage] = useState(120.1)
  const [batteryCharge, setBatteryCharge] = useState(100)
  const [batteryVoltage, setBatteryVoltage] = useState(54.6)
  const [batteryError, setBatteryError] = useState(0)
  const [loadPct, setLoadPct] = useState(23)
  const [efficiency, setEfficiency] = useState(120)
  const [outputFrequency, setOutputFrequency] = useState(60.0)
  const [sensitivity, setSensitivity] = useState<'normal' | 'reduced' | 'low'>('normal')
  const [topology, setTopology] = useState<'srt' | 'line_interactive' | 'standby'>('srt')
  const [canHE, setCanHE] = useState(true)

  const flowProps: PowerFlowProps = {
    statusRaw, inputVoltage, outputVoltage,
    batteryCharge, batteryVoltage, batteryError,
    loadPct, efficiency, outputFrequency,
    sensitivity, canHE,
  }

  const toggleBit = (bit: number) => {
    setStatusRaw(prev => prev ^ bit)
  }

  const applyPreset = (preset: Preset) => {
    setStatusRaw(preset.statusRaw)
    setInputVoltage(preset.overrides?.inputVoltage ?? 121.3)
    setOutputVoltage(preset.overrides?.outputVoltage ?? 120.1)
    setBatteryCharge(preset.overrides?.batteryCharge ?? 100)
    setBatteryVoltage(preset.overrides?.batteryVoltage ?? 54.6)
    setBatteryError(preset.overrides?.batteryError ?? 0)
    setLoadPct(preset.overrides?.loadPct ?? 23)
    setEfficiency(preset.overrides?.efficiency ?? 120)
    setOutputFrequency(preset.overrides?.outputFrequency ?? 60.0)
    setSensitivity(preset.overrides?.sensitivity as 'normal' | 'reduced' | 'low' ?? 'normal')
  }

  return (
    <div className="min-h-screen bg-page text-primary p-6">
      <div className="max-w-6xl mx-auto space-y-6">
        <div className="flex items-center justify-between">
          <div>
            <h1 className="text-xl font-semibold">PowerFlow Dev Harness</h1>
            <p className="text-xs text-muted mt-1">Adjust props below to preview all diagram states</p>
          </div>
          <a href="/" className="text-xs text-accent hover:text-accent-hover">Back to app</a>
        </div>

        {/* ── Diagram ── */}
        <div className="rounded-lg border border-edge-strong bg-panel p-2">
          <div className="flex items-center gap-3 px-3 py-2 border-b border-edge mb-2">
            <span className="text-xs text-muted uppercase tracking-wider">Topology</span>
            {(['srt', 'line_interactive', 'standby'] as const).map(t => (
              <button key={t} onClick={() => setTopology(t)}
                className={`px-3 py-1 text-xs rounded border transition-colors ${
                  topology === t
                    ? 'bg-accent border-accent text-white'
                    : 'bg-field hover:bg-field-hover border-edge-strong'
                }`}>
                {t === 'srt' ? 'Double Conversion'
                  : t === 'line_interactive' ? 'Line Interactive'
                  : 'Standby'}
              </button>
            ))}
            <label className="flex items-center gap-2 text-xs cursor-pointer ml-4">
              <input type="checkbox" checked={canHE} onChange={() => setCanHE(p => !p)}
                className="accent-accent" />
              <span className={canHE ? 'text-primary' : 'text-faint'}>HE Capable</span>
            </label>
            <span className="ml-auto text-[10px] font-mono text-faint">
              raw: 0x{statusRaw.toString(16).padStart(6, '0')} ({statusRaw})
            </span>
          </div>

          {topology === 'srt' && <PowerFlowSRT {...flowProps} />}
          {topology === 'line_interactive' && <PowerFlowLineInteractive {...flowProps} />}
          {topology === 'standby' && <PowerFlowStandby {...flowProps} />}
        </div>

        <div className="grid grid-cols-1 lg:grid-cols-2 gap-6">

          {/* ── Presets ── */}
          <div className="rounded-lg bg-panel border border-edge p-4">
            <h3 className="text-xs font-medium text-muted uppercase tracking-wider mb-3">Presets</h3>
            <div className="flex flex-wrap gap-2">
              {PRESETS.map(preset => (
                <button key={preset.label} onClick={() => applyPreset(preset)}
                  className="px-2.5 py-1 text-xs rounded border border-edge-strong bg-field hover:bg-field-hover transition-colors">
                  {preset.label}
                </button>
              ))}
            </div>
          </div>

          {/* ── Status bits ── */}
          <div className="rounded-lg bg-panel border border-edge p-4">
            <h3 className="text-xs font-medium text-muted uppercase tracking-wider mb-3">Status Bits</h3>
            <div className="grid grid-cols-3 gap-x-4 gap-y-1.5">
              {STATUS_BITS.map(({ key, bit, label }) => (
                <label key={key} className="flex items-center gap-2 text-xs cursor-pointer">
                  <input type="checkbox" checked={!!(statusRaw & bit)}
                    onChange={() => toggleBit(bit)}
                    className="accent-accent" />
                  <span className={statusRaw & bit ? 'text-primary' : 'text-faint'}>{label}</span>
                </label>
              ))}
            </div>
          </div>
        </div>

        {/* ── Analog controls ── */}
        <div className="rounded-lg bg-panel border border-edge p-4">
          <h3 className="text-xs font-medium text-muted uppercase tracking-wider mb-3">Analog Values</h3>
          <div className="grid grid-cols-1 lg:grid-cols-2 gap-x-8 gap-y-2">
            <Slider label="Input Voltage" value={inputVoltage} onChange={setInputVoltage}
              min={0} max={150} step={0.1} unit="VAC" />
            <Slider label="Output Voltage" value={outputVoltage} onChange={setOutputVoltage}
              min={0} max={150} step={0.1} unit="VAC" />
            <Slider label="Battery Charge" value={batteryCharge} onChange={setBatteryCharge}
              min={0} max={100} unit="%" />
            <Slider label="Battery Voltage" value={batteryVoltage} onChange={setBatteryVoltage}
              min={0} max={72} step={0.1} unit="VDC" />
            <Slider label="Load" value={loadPct} onChange={setLoadPct}
              min={0} max={105} unit="%" />
            <Slider label="Output Frequency" value={outputFrequency} onChange={setOutputFrequency}
              min={45} max={65} step={0.01} unit="Hz" />
            <Slider label="Efficiency" value={efficiency} onChange={setEfficiency}
              min={-1} max={128} unit="" />
            <div className="flex items-center gap-3">
              <label className="text-xs text-muted w-32 shrink-0">Battery Error</label>
              <select value={batteryError} onChange={e => setBatteryError(parseInt(e.target.value))}
                className="bg-field border border-edge-strong rounded px-2 py-1 text-xs">
                <option value={0}>None (0)</option>
                <option value={1}>Missing (1)</option>
                <option value={2}>Replace (2)</option>
                <option value={4}>Calibrating (4)</option>
              </select>
            </div>
            <div className="flex items-center gap-3">
              <label className="text-xs text-muted w-32 shrink-0">AVR Sensitivity</label>
              <select value={sensitivity} onChange={e => setSensitivity(e.target.value as 'normal' | 'reduced' | 'low')}
                className="bg-field border border-edge-strong rounded px-2 py-1 text-xs">
                <option value="normal">Normal</option>
                <option value="reduced">Reduced</option>
                <option value="low">Low</option>
              </select>
            </div>
          </div>
        </div>
      </div>
    </div>
  )
}
