import { useApi } from '../hooks/useApi'

/* ── Types ── */

interface UpsStatus {
  driver: string
  connected: boolean
  message?: string
  inventory?: {
    model: string
    serial: string
    firmware: string
    nominal_va: number
    nominal_watts: number
    sog_config: number
    freq_tolerance: number
  }
  status?: { raw: number; text: string }
  battery?: { charge_pct: number; voltage: number; runtime_sec: number }
  output?: {
    voltage: number; frequency: number; current: number
    load_pct: number; energy_wh: number
  }
  outlets?: { mog: number; sog0: number; sog1: number }
  bypass?: { voltage: number; frequency: number; status: number }
  input?: { voltage: number; status: number; transfer_high?: number; transfer_low?: number; warn_offset?: number }
  errors?: { general: number; power_system: number; battery_system: number }
  timers?: { shutdown: number; start: number; reboot: number }
  efficiency?: number
  transfer_reason?: string
  bat_test_status?: number
  rt_cal_status?: number
  bat_lifetime_status?: number
  capabilities?: string[]
  he_mode?: { inhibited: boolean; source: string }
}

/* ── Status bit constants (mirrors ups.h) ── */

const ST = {
  ONLINE:         1 << 1,
  ON_BATTERY:     1 << 2,
  BYPASS:         1 << 3,
  OUTPUT_OFF:     1 << 4,
  FAULT:          1 << 5,
  INPUT_BAD:      1 << 6,
  TEST:           1 << 7,
  PENDING_ON:     1 << 8,
  SHUT_PENDING:   1 << 9,
  COMMANDED:      1 << 10,
  HE_MODE:        1 << 13,
  FAULT_STATE:    1 << 15,
  MAINS_BAD:      1 << 19,
  FAULT_RECOVERY: 1 << 20,
  OVERLOAD:       1 << 21,
} as const

/* ── Topology helpers ── */

function hasCap(caps: string[] | undefined, name: string): boolean {
  return caps?.includes(name) ?? false
}

/* AVR state: inferred from input/output voltage differential.
 * Only meaningful for line-interactive units (SMT). */
type AvrState = 'passthrough' | 'boost' | 'buck'

function detectAvr(inputV: number, outputV: number): AvrState {
  const delta = outputV - inputV
  if (delta > 2) return 'boost'
  if (delta < -2) return 'buck'
  return 'passthrough'
}

/* Does the sog_config bitmask indicate this SOG is present? */
function hasSog(sogConfig: number, index: 0 | 1): boolean {
  return !!(sogConfig & (1 << (index + 1)))
}

/* ── Formatting helpers ── */

const fmtRuntime = (sec: number) => {
  const h = Math.floor(sec / 3600)
  const m = Math.floor((sec % 3600) / 60)
  return h > 0 ? `${h}h ${m}m` : `${m}m`
}

const fmtWatts = (loadPct: number, nominalW: number) => {
  const w = (loadPct / 100) * nominalW
  return w >= 1000 ? `${(w / 1000).toFixed(2)} kW` : `${Math.round(w)} W`
}

const fmtVA = (loadPct: number, nominalVA: number) => {
  const va = (loadPct / 100) * nominalVA
  return va >= 1000 ? `${(va / 1000).toFixed(2)} kVA` : `${Math.round(va)} VA`
}

function outletState(raw: number): { label: string; color: string } {
  if (raw & (1 << 4)) return { label: 'Shutting Down', color: 'text-yellow-400' }
  if (raw & (1 << 3)) return { label: 'Pending On', color: 'text-blue-400' }
  if (raw & (1 << 0)) return { label: 'On', color: 'text-green-400' }
  if (raw & (1 << 1)) return { label: 'Off', color: 'text-red-400' }
  return { label: 'Unknown', color: 'text-gray-500' }
}

/* ── Classifiers ── */

type PlaneStyle = { border: string; bg: string; accent: string }

/* Utility / Input plane */

type UtilityHealth = 'he' | 'online' | 'degraded' | 'down'

function classifyUtility(raw: number): UtilityHealth {
  if (raw & ST.ON_BATTERY)                        return 'down'
  if (raw & (ST.INPUT_BAD | ST.MAINS_BAD))        return 'degraded'
  if (raw & ST.HE_MODE)                           return 'he'
  return 'online'
}

const utilityStyles: Record<UtilityHealth, PlaneStyle & { label: string }> = {
  he:       { border: 'border-green-600',  bg: 'bg-green-950/40',  accent: 'text-green-400',  label: 'Utility OK -- HE Eligible' },
  online:   { border: 'border-sky-700',    bg: 'bg-sky-950/30',    accent: 'text-sky-400',    label: 'Utility Present -- Online' },
  degraded: { border: 'border-orange-600', bg: 'bg-orange-950/30', accent: 'text-orange-400', label: 'Utility Degraded' },
  down:     { border: 'border-red-600',    bg: 'bg-red-950/30',    accent: 'text-red-400',    label: 'Utility Lost -- On Battery' },
}

/* UPS plane — returns style + badge, topology-aware */

function classifyUpsPlane(raw: number, hasBypass: boolean, avr: AvrState): { style: PlaneStyle & { label: string } } {
  if (raw & (ST.FAULT | ST.FAULT_STATE))
    return { style: { border: 'border-red-600', bg: 'bg-red-950/30', accent: 'text-red-400', label: 'Fault' } }
  if (raw & ST.OUTPUT_OFF)
    return { style: { border: 'border-gray-600', bg: 'bg-gray-900/50', accent: 'text-gray-400', label: 'Output Off' } }

  /* Bypass (double-conversion only) */
  if (hasBypass && (raw & ST.BYPASS)) {
    if (raw & ST.COMMANDED)
      return { style: { border: 'border-yellow-600', bg: 'bg-yellow-950/30', accent: 'text-yellow-400', label: 'Bypass (Commanded)' } }
    return { style: { border: 'border-orange-600', bg: 'bg-orange-950/30', accent: 'text-orange-400', label: 'Bypass (Fault-Forced)' } }
  }

  /* On battery — both topologies */
  if (raw & ST.ON_BATTERY)
    return { style: { border: 'border-yellow-600', bg: 'bg-yellow-950/30', accent: 'text-yellow-400', label: 'On Battery -- Inverter' } }

  /* HE mode — both topologies */
  if (raw & ST.HE_MODE)
    return { style: { border: 'border-green-600', bg: 'bg-green-950/40', accent: 'text-green-400', label: 'High Efficiency' } }

  /* Online — topology-specific labels */
  if (!hasBypass) {
    /* Line-interactive: show AVR state */
    const avrLabels: Record<AvrState, string> = {
      passthrough: 'AVR -- Passthrough',
      boost:       'AVR -- Boost',
      buck:        'AVR -- Buck',
    }
    return { style: { border: 'border-green-600', bg: 'bg-green-950/40', accent: 'text-green-400', label: avrLabels[avr] } }
  }

  /* Double-conversion: normal online */
  return { style: { border: 'border-green-600', bg: 'bg-green-950/40', accent: 'text-green-400', label: 'Double Conversion' } }
}

/* Output plane */

function classifyOutput(raw: number, loadPct: number): { style: PlaneStyle; badge: string } {
  const isFault = !!(raw & (ST.FAULT | ST.FAULT_STATE))
  const isOff = !!(raw & ST.OUTPUT_OFF)
  const isOverload = !!(raw & ST.OVERLOAD)

  if (isOff && isFault) return {
    style: { border: 'border-red-600', bg: 'bg-red-950/30', accent: 'text-red-400' },
    badge: 'Output Off (Fault)',
  }
  if (isOff) return {
    style: { border: 'border-gray-600', bg: 'bg-gray-900/50', accent: 'text-gray-400' },
    badge: 'Output Off',
  }
  if (isOverload || loadPct > 80) return {
    style: { border: 'border-orange-600', bg: 'bg-orange-950/30', accent: 'text-orange-400' },
    badge: isOverload ? 'Overload' : 'Load Critical',
  }
  if (loadPct > 60) return {
    style: { border: 'border-yellow-600', bg: 'bg-yellow-950/30', accent: 'text-yellow-400' },
    badge: 'Load Elevated',
  }
  return {
    style: { border: 'border-green-600', bg: 'bg-green-950/40', accent: 'text-green-400' },
    badge: 'Output Stage Active',
  }
}

/* ── Shared UI atoms ── */

function Metric({ label, value, unit, accent }: {
  label: string; value: string; unit?: string; accent?: string
}) {
  return (
    <div className="flex justify-between items-baseline py-1 border-b border-gray-800/60 last:border-0">
      <span className="text-xs text-gray-500">{label}</span>
      <span className={`text-sm font-mono ${accent || 'text-gray-200'}`}>
        {value}{unit && <span className="text-gray-500 ml-0.5">{unit}</span>}
      </span>
    </div>
  )
}

function BigStat({ value, unit, sub, color = 'text-white' }: {
  value: string; unit: string; sub?: string; color?: string
}) {
  return (
    <div>
      <p className={`text-2xl font-bold leading-tight ${color}`}>
        {value}<span className="text-sm font-normal text-gray-500 ml-1">{unit}</span>
      </p>
      {sub && <p className="text-xs text-gray-500 mt-0.5">{sub}</p>}
    </div>
  )
}

function Card({ title, children, className = '' }: {
  title: string; children: React.ReactNode; className?: string
}) {
  return (
    <div className={`rounded-lg bg-gray-900 border border-gray-800 ${className}`}>
      <div className="px-4 py-2 border-b border-gray-800">
        <h3 className="text-[11px] font-medium text-gray-400 uppercase tracking-wider">{title}</h3>
      </div>
      <div className="px-4 py-3">{children}</div>
    </div>
  )
}

function Plane({ title, badge, styles, children }: {
  title: string; badge: string; styles: PlaneStyle
  children: React.ReactNode
}) {
  return (
    <div className={`rounded-lg border ${styles.border} ${styles.bg}`}>
      <div className="flex items-center justify-between px-5 py-3 border-b border-white/5">
        <h3 className="text-sm font-semibold text-gray-200">{title}</h3>
        <span className={`text-xs font-medium px-2 py-0.5 rounded ${styles.accent} bg-black/30`}>{badge}</span>
      </div>
      <div className="px-5 py-4">{children}</div>
    </div>
  )
}

function OutletBadge({ label, raw }: { label: string; raw: number }) {
  const st = outletState(raw)
  const dotColor = st.color === 'text-green-400' ? 'bg-green-400'
    : st.color === 'text-red-400' ? 'bg-red-400'
    : st.color === 'text-yellow-400' ? 'bg-yellow-400' : 'bg-blue-400'
  return (
    <div className="flex items-center gap-2 py-1">
      <span className={`w-2 h-2 rounded-full ${dotColor}`} />
      <span className="text-xs text-gray-400">{label}</span>
      <span className={`text-xs font-mono ml-auto ${st.color}`}>{st.label}</span>
    </div>
  )
}

function VoltageBar({ voltage, low, high, warnOffset = 5 }: {
  voltage: number; low: number; high: number; warnOffset?: number
}) {
  const range = high - low
  const pct = Math.max(0, Math.min(100, ((voltage - low) / range) * 100))

  /* Zone boundaries based on alert thresholds */
  const warnLow = low + warnOffset
  const warnHigh = high - warnOffset

  const barColor = voltage < low || voltage > high
    ? 'bg-red-500'
    : voltage < warnLow || voltage > warnHigh
    ? 'bg-yellow-500'
    : 'bg-green-500'

  /* Warn zone markers */
  const warnLowPct = (warnOffset / range) * 100
  const warnHighPct = ((high - warnOffset - low) / range) * 100

  return (
    <div className="mt-2">
      <div className="relative h-3 rounded-full bg-gray-800 overflow-hidden">
        <div className={`absolute inset-y-0 left-0 rounded-full transition-all duration-500 ${barColor}`}
             style={{ width: `${pct}%` }} />
        {/* Warn zone ticks */}
        <div className="absolute inset-y-0 w-px bg-yellow-700/50" style={{ left: `${warnLowPct}%` }} />
        <div className="absolute inset-y-0 w-px bg-yellow-700/50" style={{ left: `${warnHighPct}%` }} />
      </div>
      <div className="flex justify-between mt-1">
        <span className="text-[10px] text-gray-600">{low}V</span>
        <span className="text-[10px] text-gray-600">{high}V</span>
      </div>
    </div>
  )
}

/* ── Main component ── */

export default function Dashboard() {
  const { data: s, error } = useApi<UpsStatus>('/api/status', 2000)

  if (error) return (
    <div className="rounded-lg bg-red-900/30 border border-red-800 p-6 text-center">
      <p className="text-red-300 text-lg mb-1">Connection Lost</p>
      <p className="text-red-400/70 text-sm">{error}</p>
    </div>
  )
  if (!s) return <LoadingPulse />
  if (!s.connected) return (
    <div className="rounded-lg bg-yellow-900/30 border border-yellow-800 p-6 text-center">
      <p className="text-yellow-200 text-lg mb-1">No UPS Connected</p>
      <p className="text-yellow-400/70 text-sm">{s.message}</p>
    </div>
  )

  const raw = s.status?.raw ?? 0
  const bat = s.battery
  const out = s.output
  const inp = s.input
  const inv = s.inventory
  const nomW = inv?.nominal_watts ?? 0
  const nomVA = inv?.nominal_va ?? 0
  const caps = s.capabilities
  const sogCfg = inv?.sog_config ?? 0

  /* Topology detection */
  const canBypass = hasCap(caps, 'bypass')
  const canHE = hasCap(caps, 'he_mode')
  const avr = detectAvr(inp?.voltage ?? 0, out?.voltage ?? 0)

  /* Classify planes */
  const utilHealth = classifyUtility(raw)
  const uStyle = utilityStyles[utilHealth]
  const { style: upStyle } = classifyUpsPlane(raw, canBypass, avr)
  const { style: outStyle, badge: outBadge } = classifyOutput(raw, out?.load_pct ?? 0)

  /* Operating mode label (topology-aware) */
  const operatingMode =
    (raw & ST.ON_BATTERY) ? 'On Battery' :
    (raw & ST.HE_MODE) ? 'High Efficiency' :
    canBypass ? ((raw & ST.BYPASS) ? 'Bypass' : 'Online / Double Conversion') :
    `Line Interactive / AVR${avr !== 'passthrough' ? ` (${avr})` : ''}`

  /* Topology label for UPS plane */
  const topologyLabel =
    (raw & ST.ON_BATTERY) ? 'Inverter (Battery)' :
    (raw & ST.HE_MODE) ? 'HE / Standby' :
    canBypass ? (
      (raw & ST.BYPASS) ? 'Bypass (straight-through)' : 'Double Conversion'
    ) : (
      avr === 'boost' ? 'AVR Boost' :
      avr === 'buck' ? 'AVR Buck' :
      'AVR Passthrough'
    )

  return (
    <div className="space-y-6">

      {/* ── Summary bar ── */}
      <div className="flex flex-wrap items-center gap-x-6 gap-y-2">
        <StatusDot raw={raw} canBypass={canBypass} />
        {inv && (
          <div className="flex flex-wrap items-center gap-x-4 gap-y-1 text-sm text-gray-400">
            <span className="text-gray-200 font-medium">{inv.model.trim()}</span>
            <span>{inv.serial.trim()}</span>
            <span>{inv.nominal_va} VA / {inv.nominal_watts} W</span>
            <span className="text-gray-500">{s.driver.toUpperCase()} driver</span>
          </div>
        )}
        {s.he_mode?.inhibited && (
          <span className="ml-auto px-2.5 py-1 rounded bg-yellow-900/50 border border-yellow-700 text-xs text-yellow-300">
            HE inhibited: {s.he_mode.source}
          </span>
        )}
      </div>

      {/* ── Overview cards ── */}
      <div className="grid grid-cols-1 md:grid-cols-2 xl:grid-cols-4 gap-4">

        {/* Battery */}
        <Card title="Battery">
          <BigStat
            value={bat?.charge_pct.toFixed(0) ?? '--'}
            unit="%"
            sub={bat ? `${fmtRuntime(bat.runtime_sec)} remaining` : undefined}
            color={bat && bat.charge_pct < 30 ? 'text-red-400' : bat && bat.charge_pct < 60 ? 'text-yellow-400' : 'text-green-400'}
          />
          <div className="mt-2">
            <Metric label="Voltage" value={bat?.voltage.toFixed(1) ?? '--'} unit="VDC" />
          </div>
        </Card>

        {/* Load */}
        <Card title="Load">
          <BigStat
            value={out?.load_pct.toFixed(1) ?? '--'}
            unit="%"
            sub={out && nomW ? fmtWatts(out.load_pct, nomW) : undefined}
            color={out && out.load_pct > 80 ? 'text-red-400' : out && out.load_pct > 60 ? 'text-yellow-400' : 'text-white'}
          />
          <div className="mt-2">
            <Metric label="Current" value={out?.current.toFixed(1) ?? '--'} unit="A" />
            <Metric label="Apparent" value={out && nomVA ? fmtVA(out.load_pct, nomVA) : '--'} />
          </div>
        </Card>

        {/* Output */}
        <Card title="Output">
          <BigStat
            value={out?.voltage.toFixed(1) ?? '--'}
            unit="VAC"
          />
          <div className="mt-2">
            <Metric label="Frequency" value={out?.frequency.toFixed(2) ?? '--'} unit="Hz" />
            {s.outlets && (
              <>
                <OutletBadge label="Main (MOG)" raw={s.outlets.mog} />
                {hasSog(sogCfg, 0) && <OutletBadge label="SOG 0" raw={s.outlets.sog0} />}
                {hasSog(sogCfg, 1) && <OutletBadge label="SOG 1" raw={s.outlets.sog1} />}
              </>
            )}
          </div>
        </Card>

        {/* Input */}
        <Card title="Input">
          <BigStat
            value={inp?.voltage.toFixed(1) ?? '--'}
            unit="VAC"
            color={uStyle.accent}
          />
          {inp?.transfer_low != null && inp?.transfer_high != null && inp.voltage > 0 && (
            <VoltageBar voltage={inp.voltage} low={inp.transfer_low} high={inp.transfer_high} warnOffset={inp.warn_offset} />
          )}
          <div className="mt-2">
            <Metric label="Last Transfer" value={humanizeTransfer(s.transfer_reason)} />
            {s.he_mode?.inhibited && (
              <Metric label="Weather Inhibit" value={s.he_mode.source} accent="text-yellow-400" />
            )}
          </div>
        </Card>
      </div>

      {/* ── Cascading power planes ── */}
      <div className="space-y-3">

        {/* Utility plane */}
        <Plane title="Utility / Input" badge={uStyle.label} styles={uStyle}>
          <div className="grid grid-cols-2 md:grid-cols-4 gap-x-6 gap-y-1">
            <Metric label="Input Voltage" value={inp?.voltage.toFixed(1) ?? '--'} unit="VAC" accent={uStyle.accent} />
            {canBypass && (
              <>
                <Metric label="Bypass Voltage" value={s.bypass?.voltage.toFixed(1) ?? '--'} unit="VAC" />
                <Metric label="Bypass Frequency" value={s.bypass?.frequency.toFixed(2) ?? '--'} unit="Hz" />
              </>
            )}
            <Metric label="Last Transfer" value={humanizeTransfer(s.transfer_reason)} />
            <Metric label="Operating Mode" value={operatingMode} accent={uStyle.accent} />
            <Metric label="Efficiency" value={s.efficiency != null && s.efficiency >= 0 ? `${s.efficiency.toFixed(1)}` : '--'} unit="%" />
            {canHE && s.he_mode?.inhibited && (
              <Metric label="HE Inhibit" value={s.he_mode.source} accent="text-yellow-400" />
            )}
            {!canBypass && avr !== 'passthrough' && (
              <Metric label="AVR Correction" value={
                inp && out ? `${(out.voltage - inp.voltage).toFixed(1)}V (${avr})` : '--'
              } accent="text-sky-400" />
            )}
          </div>
        </Plane>

        {/* UPS plane */}
        <Plane title="UPS" badge={upStyle.label} styles={upStyle}>
          <div className="grid grid-cols-2 md:grid-cols-4 gap-x-6 gap-y-1">
            <Metric label="Battery Charge" value={bat?.charge_pct.toFixed(1) ?? '--'} unit="%" accent={
              bat && bat.charge_pct < 30 ? 'text-red-400' : bat && bat.charge_pct < 60 ? 'text-yellow-400' : 'text-green-400'
            } />
            <Metric label="Battery Voltage" value={bat?.voltage.toFixed(1) ?? '--'} unit="VDC" />
            <Metric label="Runtime" value={bat ? fmtRuntime(bat.runtime_sec) : '--'} />
            <Metric label="Topology" value={topologyLabel} />
            {canBypass && (raw & ST.BYPASS) !== 0 && (
              <>
                <Metric label="Bypass Voltage" value={s.bypass?.voltage.toFixed(1) ?? '--'} unit="VAC" />
                <Metric label="Bypass Frequency" value={s.bypass?.frequency.toFixed(2) ?? '--'} unit="Hz" />
              </>
            )}
            {!canBypass && !(raw & ST.ON_BATTERY) && !(raw & ST.HE_MODE) && (
              <Metric label="AVR State" value={
                avr === 'boost' ? 'Boosting' : avr === 'buck' ? 'Bucking' : 'Passthrough'
              } accent={avr !== 'passthrough' ? 'text-sky-400' : undefined} />
            )}
            {(raw & ST.TEST) !== 0 && <Metric label="Self-Test" value="In Progress" accent="text-blue-400" />}
            {(raw & ST.SHUT_PENDING) !== 0 && <Metric label="Shutdown" value="Pending" accent="text-red-400" />}
            {(raw & ST.PENDING_ON) !== 0 && <Metric label="Startup" value="Pending" accent="text-blue-400" />}
            {(raw & ST.FAULT_RECOVERY) !== 0 && <Metric label="Recovery" value="In Progress" accent="text-yellow-400" />}
            {s.errors && s.errors.general !== 0 && <Metric label="General Error" value={`0x${s.errors.general.toString(16)}`} accent="text-red-400" />}
            {s.errors && s.errors.power_system !== 0 && <Metric label="Power Error" value={`0x${s.errors.power_system.toString(16)}`} accent="text-red-400" />}
            {s.errors && s.errors.battery_system !== 0 && <Metric label="Battery Error" value={`0x${s.errors.battery_system.toString(16)}`} accent="text-red-400" />}
          </div>
        </Plane>

        {/* Output plane */}
        <Plane title="Output" badge={outBadge} styles={outStyle}>
          <div className="grid grid-cols-2 md:grid-cols-4 gap-x-6 gap-y-1">
            <Metric label="Output Voltage" value={out?.voltage.toFixed(1) ?? '--'} unit="VAC" />
            <Metric label="Output Frequency" value={out?.frequency.toFixed(2) ?? '--'} unit="Hz" />
            <Metric label="Output Current" value={out?.current.toFixed(1) ?? '--'} unit="A" />
            <Metric label="Real Power" value={out && nomW ? fmtWatts(out.load_pct, nomW) : '--'} />
            <Metric label="Apparent Power" value={out && nomVA ? fmtVA(out.load_pct, nomVA) : '--'} />
            {out?.energy_wh != null && out.energy_wh > 0 && (
              <Metric label="Cumulative Energy" value={out.energy_wh >= 1000 ? `${(out.energy_wh / 1000).toFixed(1)} kWh` : `${out.energy_wh} Wh`} />
            )}
          </div>
          {s.outlets && (
            <div className="mt-3 pt-3 border-t border-white/5">
              <h4 className="text-[10px] font-medium text-gray-500 uppercase tracking-wider mb-1">Outlet Groups</h4>
              <div className={`grid gap-4 ${hasSog(sogCfg, 0) || hasSog(sogCfg, 1) ? 'grid-cols-3' : 'grid-cols-1'}`}>
                <OutletBadge label="Main (MOG)" raw={s.outlets.mog} />
                {hasSog(sogCfg, 0) && <OutletBadge label="SOG 0" raw={s.outlets.sog0} />}
                {hasSog(sogCfg, 1) && <OutletBadge label="SOG 1" raw={s.outlets.sog1} />}
              </div>
            </div>
          )}
          {s.timers && (s.timers.shutdown > 0 || s.timers.start > 0 || s.timers.reboot > 0) && (
            <div className="mt-3 pt-3 border-t border-white/5">
              <h4 className="text-[10px] font-medium text-gray-500 uppercase tracking-wider mb-1">Active Timers</h4>
              <div className="grid grid-cols-3 gap-4">
                {s.timers.shutdown > 0 && <Metric label="Turn Off" value={`${s.timers.shutdown}s`} accent="text-red-400" />}
                {s.timers.start > 0 && <Metric label="Turn On" value={`${s.timers.start}s`} accent="text-green-400" />}
                {s.timers.reboot > 0 && <Metric label="Stay Off" value={`${s.timers.reboot}s`} accent="text-yellow-400" />}
              </div>
            </div>
          )}
        </Plane>
      </div>
    </div>
  )
}

/* ── Sub-components ── */

function StatusDot({ raw, canBypass }: { raw: number; canBypass: boolean }) {
  const isFault = !!(raw & (ST.FAULT | ST.FAULT_STATE))
  const isOnBattery = !!(raw & ST.ON_BATTERY)
  const isBypass = canBypass && !!(raw & ST.BYPASS)
  const isCommanded = !!(raw & ST.COMMANDED)
  const isOnline = !!(raw & ST.ONLINE)

  const color = isFault ? 'bg-red-500'
    : isOnBattery ? 'bg-yellow-500'
    : isBypass ? (isCommanded ? 'bg-yellow-500' : 'bg-orange-500')
    : isOnline ? 'bg-green-500' : 'bg-gray-500'
  const label = isFault ? 'Fault'
    : isOnBattery ? 'On Battery'
    : isBypass ? (isCommanded ? 'Bypass' : 'Bypass (Forced)')
    : isOnline ? ((raw & ST.HE_MODE) ? 'Online - HE' : 'Online')
    : 'Unknown'

  return (
    <div className="flex items-center gap-2">
      <span className={`w-3 h-3 rounded-full ${color} animate-pulse`} />
      <span className="text-xl font-semibold text-gray-100">{label}</span>
    </div>
  )
}

function humanizeTransfer(reason?: string): string {
  if (!reason) return '--'
  const map: Record<string, string> = {
    SystemInitialization: 'System Init',
    HighInputVoltage: 'High Voltage',
    LowInputVoltage: 'Low Voltage',
    DistortedInput: 'Distorted Input',
    RapidChangeOfInputVoltage: 'Rapid Voltage Change',
    HighInputFrequency: 'High Frequency',
    LowInputFrequency: 'Low Frequency',
    FreqAndOrPhaseDifference: 'Freq/Phase Delta',
    AcceptableInput: 'Input Acceptable',
    AutomaticTest: 'Auto Test',
    TestEnded: 'Test Ended',
    LocalUICommand: 'Front Panel',
    ProtocolCommand: 'Protocol Command',
    LowBatteryVoltage: 'Low Battery',
    GeneralError: 'General Error',
    PowerSystemError: 'Power Error',
    BatterySystemError: 'Battery Error',
    ErrorCleared: 'Error Cleared',
    AutomaticRestart: 'Auto Restart',
    ConfigurationChange: 'Config Change',
  }
  return map[reason] ?? reason
}

function LoadingPulse() {
  return (
    <div className="space-y-4">
      <div className="h-8 w-64 bg-gray-800 rounded animate-pulse" />
      <div className="grid grid-cols-1 md:grid-cols-2 xl:grid-cols-4 gap-4">
        {[1,2,3,4].map(i => <div key={i} className="rounded-lg bg-gray-900 border border-gray-800 h-32 animate-pulse" />)}
      </div>
      <div className="space-y-3">
        {[1,2,3].map(i => <div key={i} className="rounded-lg bg-gray-900 border border-gray-800 h-24 animate-pulse" />)}
      </div>
    </div>
  )
}
