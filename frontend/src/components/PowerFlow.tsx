/* Power flow single-line diagram.
 *
 * SVG schematic showing the UPS power path with animated flow lines
 * and color-coded stage blocks. Topology-aware — renders the correct
 * diagram for double-conversion, line-interactive, or standby units.
 *
 * SRT Double Conversion layout:
 *
 *   ┌───────┐    ┌───────────┐    ┌────────┐    ┌──────────┐    ┌────────┐
 *   │ Input │───►│ Rectifier │───►│ DC Bus │───►│ Inverter │───►│ Output │
 *   └───────┘    └───────────┘    └───┬────┘    └──────────┘    └────────┘
 *       │                             │                              ▲
 *       │                        ┌────▼────┐                         │
 *       │                        │ Battery │                         │
 *       │                        └─────────┘                         │
 *       │                                                            │
 *       └────────────── Static Bypass ──────────────────────────────►┘
 */

import { ST } from '../types/ups'
import type { PowerFlowProps } from '../types/ups'

/* ── Color palette ── */

/* Status colors are fixed (they need to read on both light and dark).
 * Chrome colors read from CSS variables so they follow the theme. */
function getCssVar(name: string): string {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim()
}

function useThemeColors() {
  /* Re-read on every render — cheap, and catches theme changes */
  return {
    active:      '#22c55e',
    he:          '#22c55e',
    bypass:      '#eab308',
    bypassF:     '#f97316',
    battery:     '#eab308',
    fault:       '#ef4444',
    heStandby:   '#166534',
    inactive:    getCssVar('--color-edge-strong') || '#404040',
    dead:        getCssVar('--color-edge') || '#262626',
    block:       getCssVar('--color-panel') || '#171717',
    blockBorder: getCssVar('--color-edge-strong') || '#404040',
    text:        getCssVar('--color-secondary') || '#d4d4d4',
    textDim:     getCssVar('--color-faint') || '#8a8a8a',
    textBright:  getCssVar('--color-primary') || '#f5f5f5',
  }
}


/* ── Animated flow line ── */

function FlowLine({ points, color, active, reverse, slow }: {
  points: string
  color: string
  active: boolean
  reverse?: boolean
  slow?: boolean
}) {
  const C = useThemeColors()
  return (
    <g>
      {/* Background line */}
      <polyline
        points={points}
        fill="none"
        stroke={active ? color : C.inactive}
        strokeWidth="2"
        strokeLinecap="round"
        strokeLinejoin="round"
        opacity={active ? 0.3 : 0.5}
      />
      {/* Animated dashes on top */}
      {active && (
        <polyline
          points={points}
          fill="none"
          stroke={color}
          strokeWidth="2"
          strokeLinecap="round"
          strokeLinejoin="round"
          strokeDasharray="6 8"
          className={reverse
            ? (slow ? 'animate-flow-reverse-slow' : 'animate-flow-reverse')
            : (slow ? 'animate-flow-slow' : 'animate-flow')
          }
        />
      )}
    </g>
  )
}

/* ── Stage block ── */

function StageBlock({ x, y, w, h, label, value, unit, color, borderColor }: {
  x: number; y: number; w: number; h: number
  label: string
  value?: string
  unit?: string
  color?: string
  borderColor?: string
}) {
  const C = useThemeColors()
  return (
    <g>
      <rect
        x={x} y={y} width={w} height={h}
        rx="4" ry="4"
        fill={C.block}
        stroke={borderColor || C.blockBorder}
        strokeWidth="1.5"
      />
      <text
        x={x + w / 2} y={y + (value ? 16 : h / 2 + 4)}
        textAnchor="middle"
        fill={C.textDim}
        fontSize="9"
        fontWeight="500"
      >
        {label}
      </text>
      {value && (
        <text
          x={x + w / 2} y={y + 32}
          textAnchor="middle"
          fill={color || C.textBright}
          fontSize="13"
          fontWeight="600"
          fontFamily="ui-monospace, monospace"
        >
          {value}{unit && <tspan fill={C.textDim} fontSize="9"> {unit}</tspan>}
        </text>
      )}
    </g>
  )
}

/* ── Arrow head marker ── */

function ArrowDefs() {
  const C = useThemeColors()
  return (
    <defs>
      <marker id="arrow-active" viewBox="0 0 6 6" refX="5" refY="3"
        markerWidth="5" markerHeight="5" orient="auto-start-reverse">
        <path d="M0,0 L6,3 L0,6 Z" fill={C.active} />
      </marker>
      <marker id="arrow-inactive" viewBox="0 0 6 6" refX="5" refY="3"
        markerWidth="5" markerHeight="5" orient="auto-start-reverse">
        <path d="M0,0 L6,3 L0,6 Z" fill={C.inactive} />
      </marker>
    </defs>
  )
}

/* TODO:
 * - Center bypass label horizontally in Double Conversion topology
 * - Center bypass label between input and transfer boxes on Line Interactive
 * - Fix scaling to make it more readable for tall flows like Line Interactive and Standby
 */

/* ── SRT Double Conversion Diagram ── */

export function PowerFlowSRT({
  statusRaw: raw, inputVoltage, outputVoltage,
  batteryCharge, batteryVoltage, batteryError,
  loadPct, outputFrequency,
}: PowerFlowProps) {
  const C = useThemeColors()

  const isOnline = !!(raw & ST.ONLINE)
  const isOnBattery = !!(raw & ST.ON_BATTERY)
  const isBypass = !!(raw & ST.BYPASS)
  const isCommanded = !!(raw & ST.COMMANDED)
  const isHE = !!(raw & ST.HE_MODE)
  const isFault = !!(raw & (ST.FAULT | ST.FAULT_STATE))
  const isOff = !!(raw & ST.OUTPUT_OFF)

  /* Determine path states */
  const batFault = batteryError !== 0
  const inputActive = isOnline || isBypass || isHE
  const rectifierActive = isOnline && !isBypass && !isHE && !isOff
  const dcBusActive = rectifierActive || isOnBattery
  const inverterActive = dcBusActive && !isOff
  const inverterTracking = isHE && !isOff
  const bypassActive = (isBypass || isHE) && !isOff
  const batteryCharging = inputActive && !isOnBattery && !batFault && batteryCharge < 100
  const batteryDischarging = isOnBattery && !batFault

  /* Path colors */
  const mainColor = isFault ? C.fault : C.active
  const bypassColor = isFault ? C.fault : isHE ? C.he : isCommanded ? C.bypass : C.bypassF
  const batteryColor = isOnBattery ? C.battery : C.active

  /* Block border colors based on state */
  const inputBorder = inputActive ? mainColor : isFault ? C.fault : C.inactive
  const rectBorder = (rectifierActive || batteryCharging) ? mainColor : inverterTracking ? C.heStandby : C.inactive
  const dcBorder = (dcBusActive || batteryCharging) ? mainColor : inverterTracking ? C.heStandby : C.inactive
  const invBorder = inverterActive ? mainColor : inverterTracking ? C.heStandby : C.inactive
  const outBorder = (inverterActive || bypassActive) && !isOff ? mainColor : isOff ? C.dead : C.inactive
  const batBorder = batFault ? C.fault : (batteryCharging || batteryDischarging) ? batteryColor : C.inactive

  /* Layout constants */
  const W = 650, H = 200
  const bw = 80, bh = 44  /* block width/height */
  const bypassY = 14      /* bypass path Y position (above main) */

  /* Block positions — main path centered, bypass above, battery below */
  const input   = { x: 10,  y: 60 }
  const rect    = { x: 140, y: 60 }
  const dcbus   = { x: 270, y: 60 }
  const inv     = { x: 400, y: 60 }
  const output  = { x: 530, y: 60 }
  const battery = { x: 270, y: 138 }

  /* Connection points (center edges) */
  const cx = (b: {x: number; y: number}) => ({ l: b.x, r: b.x + bw, cx: b.x + bw/2, t: b.y, b: b.y + bh, cy: b.y + bh/2 })
  const iC = cx(input), rC = cx(rect), dC = cx(dcbus), nC = cx(inv), oC = cx(output), bC = cx(battery)

  const fmtV = (v: number) => v > 0 ? v.toFixed(1) : '--'
  const fmtPct = (v: number) => v >= 0 ? v.toFixed(0) : '--'

  return (
    <div className="rounded-lg bg-panel border border-edge p-4">
      <svg viewBox={`0 0 ${W} ${H}`} className="w-full" style={{ maxHeight: '210px' }}>
        <ArrowDefs />

        {/* ── Flow lines ── */}

        {/* Input → Rectifier */}
        <FlowLine
          points={`${iC.r},${iC.cy} ${rC.l},${rC.cy}`}
          color={mainColor} active={rectifierActive || batteryCharging}
          slow={!rectifierActive && batteryCharging}
        />

        {/* Rectifier → DC Bus */}
        <FlowLine
          points={`${rC.r},${rC.cy} ${dC.l},${dC.cy}`}
          color={mainColor} active={rectifierActive || batteryCharging}
          slow={!rectifierActive && batteryCharging}
        />

        {/* DC Bus → Inverter */}
        <FlowLine
          points={`${dC.r},${dC.cy} ${nC.l},${nC.cy}`}
          color={mainColor} active={dcBusActive && inverterActive}
        />

        {/* Inverter → Output */}
        <FlowLine
          points={`${nC.r},${nC.cy} ${oC.l},${oC.cy}`}
          color={mainColor} active={inverterActive}
        />

        {/* DC Bus ↔ Battery */}
        <FlowLine
          points={`${dC.cx},${dC.b} ${bC.cx},${bC.t}`}
          color={batteryColor}
          active={batteryCharging || batteryDischarging}
          reverse={batteryDischarging}
          slow={batteryCharging}
        />

        {/* Bypass path: Input → Output (arcs over the top) */}
        <FlowLine
          points={`${iC.cx},${iC.t} ${iC.cx},${bypassY} ${oC.cx},${bypassY} ${oC.cx},${oC.t}`}
          color={bypassColor}
          active={bypassActive}
        />

        {/* Bypass label */}
        <text
          x={(iC.cx + oC.cx) / 2} y={bypassY - 3}
          textAnchor="middle"
          fill={bypassActive ? bypassColor : C.textDim}
          fontSize="8"
          fontWeight="500"
          opacity={0.8}
          dx="-20"
        >
          {bypassActive ? (isHE ? 'HE BYPASS' : isCommanded ? 'MAINTENANCE BYPASS' : 'FAULT BYPASS') : 'BYPASS'}
        </text>

        {/* ── Stage blocks ── */}

        <StageBlock {...input} w={bw} h={bh} label="INPUT"
          value={fmtV(inputVoltage)} unit="VAC"
          borderColor={inputBorder}
          color={inputActive ? C.textBright : C.textDim}
        />

        <StageBlock {...rect} w={bw} h={bh} label="RECTIFIER"
          borderColor={rectBorder}
        />

        <StageBlock {...dcbus} w={bw} h={bh} label="DC BUS"
          borderColor={dcBorder}
        />

        <StageBlock {...inv} w={bw} h={bh} label="INVERTER"
          value={inverterTracking ? 'Tracking' : inverterActive ? fmtV(outputFrequency) : undefined}
          unit={inverterActive ? 'Hz' : undefined}
          borderColor={invBorder}
          color={inverterTracking ? C.heStandby : undefined}
        />

        {/* HE mode: dotted lines across the conversion chain — tracking but not delivering.
         * Skip segments that have active charging flow lines to avoid visual overlap. */}
        {inverterTracking && (
          <>
            {!batteryCharging && (
              <>
                <line
                  x1={iC.r} y1={iC.cy} x2={rC.l} y2={rC.cy}
                  stroke={C.heStandby} strokeWidth="1.5" strokeDasharray="3 4" opacity="0.6"
                />
                <line
                  x1={rC.r} y1={rC.cy} x2={dC.l} y2={dC.cy}
                  stroke={C.heStandby} strokeWidth="1.5" strokeDasharray="3 4" opacity="0.6"
                />
              </>
            )}
            <line
              x1={dC.r} y1={dC.cy} x2={nC.l} y2={nC.cy}
              stroke={C.heStandby} strokeWidth="1.5" strokeDasharray="3 4" opacity="0.6"
            />
            <line
              x1={nC.r} y1={nC.cy} x2={oC.l} y2={oC.cy}
              stroke={C.heStandby} strokeWidth="1.5" strokeDasharray="3 4" opacity="0.6"
            />
          </>
        )}

        <StageBlock {...output} w={bw} h={bh} label="OUTPUT"
          value={`${fmtV(outputVoltage)}`} unit="VAC"
          borderColor={outBorder}
          color={(inverterActive || bypassActive) && !isOff ? C.textBright : C.textDim}
        />

        <StageBlock {...battery} w={bw} h={bh} label="BATTERY"
          value={batFault ? 'Missing' : `${fmtPct(batteryCharge)}%`}
          borderColor={batBorder}
          color={batFault ? C.fault :
                 batteryCharge < 30 ? C.fault :
                 batteryDischarging ? C.battery :
                 batteryCharge < 60 ? C.bypass :
                 C.active}
        />

        {/* Load badge below output */}
        <text
          x={oC.cx} y={output.y + bh + 14}
          textAnchor="middle"
          fill={loadPct > 80 ? C.fault : loadPct > 60 ? C.bypass : C.textDim}
          fontSize="8"
        >
          {fmtPct(loadPct)}% load
        </text>

        {/* Battery voltage below battery block */}
        {!batFault && (
          <text
            x={bC.cx} y={battery.y + bh + 14}
            textAnchor="middle"
            fill={C.textDim}
            fontSize="8"
          >
            {fmtV(batteryVoltage)} VDC
          </text>
        )}

      </svg>
    </div>
  )
}

/* ── Line-Interactive Diagram ──
 *
 *            ── HE Bypass ──
 *           │               │
 *   ┌───────┐    ┌─────┐    ┌──────────┐    ┌────────┐
 *   │ Input │───►│ AVR │───►│ Transfer │───►│ Output │
 *   └───────┘    └─────┘    └────┬─────┘    └────────┘
 *                                │
 *                           ┌────▼────┐
 *                           │Inverter │
 *                           └────┬────┘
 *                                │
 *                           ┌────▼────┐
 *                           │ Battery │
 *                           └─────────┘
 *
 * Normal: input → AVR → transfer switch → output.
 *         Inverter charges battery from AC line.
 * On battery: transfer switch flips, inverter feeds load from battery.
 * HE mode: bypass arc skips AVR, input → transfer switch directly.
 * AVR states: Normal (passthrough), Boost (step up), Trim (step down).
 */

export function PowerFlowLineInteractive({
  statusRaw: raw, inputVoltage, outputVoltage,
  batteryCharge, batteryVoltage, batteryError,
  loadPct, outputFrequency: _outputFrequency, canHE,
}: PowerFlowProps) {
  const C = useThemeColors()

  const isOnline = !!(raw & ST.ONLINE)
  const isOnBattery = !!(raw & ST.ON_BATTERY)
  const isHE = !!(raw & ST.HE_MODE)
  const isFault = !!(raw & (ST.FAULT | ST.FAULT_STATE))
  const isOff = !!(raw & ST.OUTPUT_OFF)
  const isBoost = !!(raw & ST.AVR_BOOST)
  const isTrim = !!(raw & ST.AVR_TRIM)

  const batFault = batteryError !== 0

  /* Path states */
  const utilityPath = isOnline && !isOnBattery && !isOff
  const batteryPath = isOnBattery && !isOff
  const chargingOnly = isOnline && !isOnBattery && isOff && !batFault && batteryCharge < 100
  const avrActive = utilityPath && !isHE
  const avrCorrecting = avrActive && (isBoost || isTrim)
  const avrTracking = isHE && isOnline
  const heBypass = (canHE && isHE && !isOnBattery) || chargingOnly
  const batteryCharging = isOnline && !isOnBattery && !batFault && batteryCharge < 100
  const batteryDischarging = isOnBattery && !batFault
  const inverterCharging = batteryCharging
  const inverterInverting = batteryPath

  /* Colors */
  const mainColor = isFault ? C.fault : C.active
  const avrColor = isFault ? C.fault : avrCorrecting ? C.battery : C.active
  const heColor = isFault ? C.fault : C.he
  const batColor = isOnBattery ? C.battery : C.active

  /* Block borders */
  const inputBorder = (isOnline || chargingOnly) ? mainColor : isFault ? C.fault : C.inactive
  const avrBorder = avrActive ? avrColor : avrTracking ? C.heStandby : C.inactive
  const switchBorder = (utilityPath || batteryPath || chargingOnly) ? mainColor : C.inactive
  const outBorder = (utilityPath || batteryPath) && !isOff ? mainColor : isOff ? C.dead : C.inactive
  const invBorder = (inverterCharging || inverterInverting) ? batColor : C.inactive
  const batBorder = batFault ? C.fault : (batteryCharging || batteryDischarging) ? batColor : C.inactive

  /* Layout */
  const W = 530, H = 280
  const bw = 80, bh = 44
  const heBypassY = 14

  /* Block positions */
  const input    = { x: 10,  y: 60 }
  const avr      = { x: 140, y: 60 }
  const xswitch  = { x: 270, y: 60 }
  const output   = { x: 400, y: 60 }
  const inverter = { x: 270, y: 138 }
  const battery  = { x: 270, y: 216 }

  const cx = (b: {x: number; y: number}) => ({ l: b.x, r: b.x + bw, cx: b.x + bw/2, t: b.y, b: b.y + bh, cy: b.y + bh/2 })
  const iC = cx(input), aC = cx(avr), sC = cx(xswitch), oC = cx(output), nC = cx(inverter), bC = cx(battery)

  const fmtV = (v: number) => v > 0 ? v.toFixed(1) : '--'
  const fmtPct = (v: number) => v >= 0 ? v.toFixed(0) : '--'

  /* AVR display value */
  const avrValue = avrTracking ? 'Tracking' : isBoost ? 'Boost' : isTrim ? 'Trim' : avrActive ? 'Normal' : undefined

  return (
    <div className="rounded-lg bg-panel border border-edge p-4">
      <svg viewBox={`0 0 ${W} ${H}`} className="w-full" style={{ maxHeight: '270px' }}>
        <ArrowDefs />

        {/* ── Flow lines ── */}

        {/* Input → AVR */}
        <FlowLine
          points={`${iC.r},${iC.cy} ${aC.l},${aC.cy}`}
          color={avrColor} active={avrActive}
        />

        {/* AVR → Transfer Switch */}
        <FlowLine
          points={`${aC.r},${aC.cy} ${sC.l},${sC.cy}`}
          color={mainColor} active={avrActive}
        />

        {/* HE bypass: Input → Transfer Switch (arc over AVR) — only shown
           * when the UPS supports HE mode, or during charging-only state */}
        {(canHE || chargingOnly) && (
          <>
            <FlowLine
              points={`${iC.cx},${iC.t} ${iC.cx},${heBypassY} ${sC.cx},${heBypassY} ${sC.cx},${sC.t}`}
              color={heColor}
              active={heBypass}
              slow={chargingOnly}
            />
            <text
              x={(iC.cx + sC.cx) / 2} y={heBypassY - 3}
              textAnchor="middle"
              fill={heBypass ? heColor : C.textDim}
              fontSize="8"
              fontWeight="500"
              opacity={0.8}
              dx="-10"
            >
              {heBypass ? (chargingOnly ? 'BYPASS' : 'HE BYPASS') : 'BYPASS'}
            </text>
          </>
        )}

        {/* AVR tracking dotted line (HE mode) */}
        {avrTracking && (
          <>
            <line
              x1={iC.r} y1={iC.cy} x2={aC.l} y2={aC.cy}
              stroke={C.heStandby}
              strokeWidth="1.5"
              strokeDasharray="3 4"
              opacity="0.6"
            />
            <line
              x1={aC.r} y1={aC.cy} x2={sC.l} y2={sC.cy}
              stroke={C.heStandby}
              strokeWidth="1.5"
              strokeDasharray="3 4"
              opacity="0.6"
            />
          </>
        )}

        {/* Transfer Switch → Output */}
        <FlowLine
          points={`${sC.r},${sC.cy} ${oC.l},${oC.cy}`}
          color={batteryPath ? batColor : mainColor}
          active={utilityPath || batteryPath}
        />

        {/* Transfer Switch ↔ Inverter */}
        <FlowLine
          points={`${sC.cx},${sC.b} ${nC.cx},${nC.t}`}
          color={batColor}
          active={inverterCharging || inverterInverting}
          reverse={inverterInverting}
          slow={inverterCharging}
        />

        {/* Inverter ↔ Battery */}
        <FlowLine
          points={`${nC.cx},${nC.b} ${bC.cx},${bC.t}`}
          color={batColor}
          active={batteryCharging || batteryDischarging}
          reverse={batteryDischarging}
          slow={batteryCharging}
        />

        {/* ── Stage blocks ── */}

        <StageBlock {...input} w={bw} h={bh} label="INPUT"
          value={fmtV(inputVoltage)} unit="VAC"
          borderColor={inputBorder}
          color={isOnline ? C.textBright : C.textDim}
        />

        <StageBlock {...avr} w={bw} h={bh} label="AVR"
          value={avrValue}
          borderColor={avrBorder}
          color={avrTracking ? C.heStandby : avrCorrecting ? C.battery : undefined}
        />

        <StageBlock {...xswitch} w={bw} h={bh} label="TRANSFER"
          value={isOnBattery ? 'Battery' : 'Utility'}
          borderColor={switchBorder}
          color={isOnBattery ? C.battery : utilityPath ? C.textBright : C.textDim}
        />

        <StageBlock {...output} w={bw} h={bh} label="OUTPUT"
          value={fmtV(outputVoltage)} unit="VAC"
          borderColor={outBorder}
          color={(utilityPath || batteryPath) && !isOff ? C.textBright : C.textDim}
        />

        <StageBlock {...inverter} w={bw} h={bh} label="INVERTER"
          value={inverterInverting ? 'On' : inverterCharging ? 'Charging' : undefined}
          borderColor={invBorder}
          color={inverterInverting ? C.battery : undefined}
        />

        <StageBlock {...battery} w={bw} h={bh} label="BATTERY"
          value={batFault ? 'Missing' : `${fmtPct(batteryCharge)}%`}
          borderColor={batBorder}
          color={batFault ? C.fault :
                 batteryCharge < 30 ? C.fault :
                 batteryDischarging ? C.battery :
                 batteryCharge < 60 ? C.bypass :
                 C.active}
        />

        {/* Load badge below output */}
        <text
          x={oC.cx} y={output.y + bh + 14}
          textAnchor="middle"
          fill={loadPct > 80 ? C.fault : loadPct > 60 ? C.bypass : C.textDim}
          fontSize="8"
        >
          {fmtPct(loadPct)}% load
        </text>

        {/* Battery voltage below battery block */}
        {!batFault && (
          <text
            x={bC.cx} y={battery.y + bh + 14}
            textAnchor="middle"
            fill={C.textDim}
            fontSize="8"
          >
            {fmtV(batteryVoltage)} VDC
          </text>
        )}

      </svg>
    </div>
  )
}

/* ── Standby / Offline Diagram ──
 *
 *   ┌───────┐    ┌──────────┐    ┌────────┐
 *   │ Input │───►│ Transfer │───►│ Output │
 *   └───────┘    └────┬─────┘    └────────┘
 *                     │
 *                ┌────▼────┐
 *                │Inverter │
 *                └────┬────┘
 *                     │
 *                ┌────▼────┐
 *                │ Battery │
 *                └─────────┘
 *
 * On utility: input passes straight through to output.
 *             Inverter charges battery from AC line.
 * On battery: transfer switch flips, battery → inverter → output.
 */

export function PowerFlowStandby({
  statusRaw: raw, inputVoltage, outputVoltage,
  batteryCharge, batteryVoltage, batteryError,
  loadPct,
}: PowerFlowProps) {
  const C = useThemeColors()

  const isOnline = !!(raw & ST.ONLINE)
  const isOnBattery = !!(raw & ST.ON_BATTERY)
  const isFault = !!(raw & (ST.FAULT | ST.FAULT_STATE))
  const isOff = !!(raw & ST.OUTPUT_OFF)
  const batFault = batteryError !== 0

  /* Path states */
  const utilityPath = isOnline && !isOnBattery && !isOff
  const batteryPath = isOnBattery && !isOff
  const chargingOnly = isOnline && !isOnBattery && isOff && !batFault && batteryCharge < 100
  const batteryCharging = (isOnline && !isOnBattery && !batFault && batteryCharge < 100)
  const batteryDischarging = isOnBattery && !batFault
  const inverterCharging = batteryCharging
  const inverterInverting = batteryPath

  /* Colors */
  const mainColor = isFault ? C.fault : C.active
  const batColor = isOnBattery ? C.battery : C.active

  /* Block borders */
  const inputBorder = (isOnline || chargingOnly) ? mainColor : isFault ? C.fault : C.inactive
  const outBorder = (utilityPath || batteryPath) ? mainColor : isOff ? C.dead : C.inactive
  const switchBorder = (utilityPath || batteryPath || chargingOnly) ? mainColor : C.inactive
  const invBorder = (inverterCharging || inverterInverting) ? batColor : C.inactive
  const batBorder = batFault ? C.fault : (batteryCharging || batteryDischarging) ? batColor : C.inactive

  /* Layout */
  const W = 400, H = 280
  const bw = 80, bh = 44

  const input    = { x: 10,  y: 60 }
  const xswitch  = { x: 140, y: 60 }
  const output   = { x: 270, y: 60 }
  const inverter = { x: 140, y: 138 }
  const battery  = { x: 140, y: 216 }

  const cx = (b: {x: number; y: number}) => ({ l: b.x, r: b.x + bw, cx: b.x + bw/2, t: b.y, b: b.y + bh, cy: b.y + bh/2 })
  const iC = cx(input), sC = cx(xswitch), oC = cx(output), nC = cx(inverter), bC = cx(battery)

  const fmtV = (v: number) => v > 0 ? v.toFixed(1) : '--'
  const fmtPct = (v: number) => v >= 0 ? v.toFixed(0) : '--'

  return (
    <div className="rounded-lg bg-panel border border-edge p-4">
      <svg viewBox={`0 0 ${W} ${H}`} className="w-full" style={{ maxHeight: '270px' }}>

        {/* Input → Transfer Switch */}
        <FlowLine
          points={`${iC.r},${iC.cy} ${sC.l},${sC.cy}`}
          color={mainColor} active={utilityPath || batteryCharging}
          slow={chargingOnly}
        />

        {/* Transfer Switch → Output */}
        <FlowLine
          points={`${sC.r},${sC.cy} ${oC.l},${oC.cy}`}
          color={batteryPath ? batColor : mainColor}
          active={utilityPath || batteryPath}
        />

        {/* Transfer Switch ↔ Inverter */}
        <FlowLine
          points={`${sC.cx},${sC.b} ${nC.cx},${nC.t}`}
          color={batColor}
          active={inverterCharging || inverterInverting}
          reverse={inverterInverting}
          slow={inverterCharging}
        />

        {/* Inverter ↔ Battery */}
        <FlowLine
          points={`${nC.cx},${nC.b} ${bC.cx},${bC.t}`}
          color={batColor}
          active={batteryCharging || batteryDischarging}
          reverse={batteryDischarging}
          slow={batteryCharging}
        />

        {/* Stage blocks */}
        <StageBlock {...input} w={bw} h={bh} label="INPUT"
          value={fmtV(inputVoltage)} unit="VAC"
          borderColor={inputBorder}
          color={isOnline ? C.textBright : C.textDim}
        />

        <StageBlock {...xswitch} w={bw} h={bh} label="TRANSFER"
          value={isOnBattery ? 'Battery' : 'Utility'}
          borderColor={switchBorder}
          color={isOnBattery ? C.battery : utilityPath ? C.textBright : C.textDim}
        />

        <StageBlock {...output} w={bw} h={bh} label="OUTPUT"
          value={fmtV(outputVoltage)} unit="VAC"
          borderColor={outBorder}
          color={(utilityPath || batteryPath) ? C.textBright : C.textDim}
        />

        <StageBlock {...inverter} w={bw} h={bh} label="INVERTER"
          value={inverterInverting ? 'On' : inverterCharging ? 'Charging' : undefined}
          borderColor={invBorder}
          color={inverterInverting ? C.battery : undefined}
        />

        <StageBlock {...battery} w={bw} h={bh} label="BATTERY"
          value={batFault ? 'Missing' : `${fmtPct(batteryCharge)}%`}
          borderColor={batBorder}
          color={batFault ? C.fault :
                 batteryCharge < 30 ? C.fault :
                 batteryDischarging ? C.battery :
                 batteryCharge < 60 ? C.bypass :
                 C.active}
        />

        {/* Load badge below output */}
        <text
          x={oC.cx} y={output.y + bh + 14}
          textAnchor="middle"
          fill={loadPct > 80 ? C.fault : loadPct > 60 ? C.bypass : C.textDim}
          fontSize="8"
        >
          {fmtPct(loadPct)}% load
        </text>

        {/* Battery voltage below battery block */}
        {!batFault && (
          <text
            x={bC.cx} y={battery.y + bh + 14}
            textAnchor="middle"
            fill={C.textDim}
            fontSize="8"
          >
            {fmtV(batteryVoltage)} VDC
          </text>
        )}

      </svg>
    </div>
  )
}
