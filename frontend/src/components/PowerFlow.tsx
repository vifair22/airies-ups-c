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

const ST = {
  ONLINE:       1 << 1,
  ON_BATTERY:   1 << 2,
  BYPASS:       1 << 3,
  OUTPUT_OFF:   1 << 4,
  FAULT:        1 << 5,
  COMMANDED:    1 << 10,
  HE_MODE:      1 << 13,
  FAULT_STATE:  1 << 15,
} as const

/* ── Color palette ── */

const C = {
  active:   '#22c55e',   /* green-500 */
  he:       '#22c55e',   /* green for HE (bypass is intentional) */
  bypass:   '#eab308',   /* yellow-500 */
  bypassF:  '#f97316',   /* orange-500 for forced bypass */
  battery:  '#eab308',   /* yellow-500 */
  fault:    '#ef4444',   /* red-500 */
  heStandby: '#166534',  /* green-800 — inverter tracking in HE mode */
  inactive: '#374151',   /* gray-700 */
  dead:     '#1f2937',   /* gray-800 */
  block:    '#111827',   /* panel */
  blockBorder: '#374151', /* gray-700 */
  text:     '#d1d5db',   /* secondary */
  textDim:  '#6b7280',   /* faint */
  textBright: '#f3f4f6', /* primary */
}

/* ── Animated flow line ── */

function FlowLine({ points, color, active, reverse }: {
  points: string
  color: string
  active: boolean
  reverse?: boolean
}) {
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
          className={reverse ? 'animate-flow-reverse' : 'animate-flow'}
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

/* ── SRT Double Conversion Diagram ── */

interface PowerFlowProps {
  statusRaw: number
  inputVoltage: number
  outputVoltage: number
  batteryCharge: number
  batteryVoltage: number
  batteryError: number
  loadPct: number
  efficiency: number
  outputFrequency: number
}

export function PowerFlowSRT({
  statusRaw: raw, inputVoltage, outputVoltage,
  batteryCharge, batteryVoltage, batteryError,
  loadPct, efficiency, outputFrequency,
}: PowerFlowProps) {

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
  const bypassActive = isBypass || isHE
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
  const W = 620, H = 210
  const bw = 80, bh = 44  /* block width/height */
  const bypassY = 14      /* bypass path Y position (above main) */

  /* Block positions — main path centered, bypass above, battery below */
  const input   = { x: 10,  y: 60 }
  const rect    = { x: 130, y: 60 }
  const dcbus   = { x: 250, y: 60 }
  const inv     = { x: 370, y: 60 }
  const output  = { x: 530, y: 60 }
  const battery = { x: 250, y: 150 }

  /* Connection points (center edges) */
  const cx = (b: {x: number; y: number}) => ({ l: b.x, r: b.x + bw, cx: b.x + bw/2, t: b.y, b: b.y + bh, cy: b.y + bh/2 })
  const iC = cx(input), rC = cx(rect), dC = cx(dcbus), nC = cx(inv), oC = cx(output), bC = cx(battery)

  const fmtV = (v: number) => v > 0 ? v.toFixed(1) : '--'
  const fmtPct = (v: number) => v >= 0 ? v.toFixed(0) : '--'

  return (
    <div className="rounded-lg bg-panel border border-edge p-4">
      <svg viewBox={`0 0 ${W} ${H}`} className="w-full" style={{ maxHeight: '220px' }}>
        <ArrowDefs />

        {/* ── Flow lines ── */}

        {/* Input → Rectifier */}
        <FlowLine
          points={`${iC.r},${iC.cy} ${rC.l},${rC.cy}`}
          color={mainColor} active={rectifierActive || batteryCharging}
        />

        {/* Rectifier → DC Bus */}
        <FlowLine
          points={`${rC.r},${rC.cy} ${dC.l},${dC.cy}`}
          color={mainColor} active={rectifierActive || batteryCharging}
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
        />

        {/* Bypass path: Input → Output (arcs over the top) */}
        <FlowLine
          points={`${iC.cx},${iC.t} ${iC.cx},${bypassY} ${oC.cx},${bypassY} ${oC.cx},${oC.t}`}
          color={bypassColor}
          active={bypassActive}
        />

        {/* Bypass label */}
        <text
          x={W / 2} y={bypassY - 3}
          textAnchor="middle"
          fill={bypassActive ? bypassColor : C.textDim}
          fontSize="8"
          fontWeight="500"
          opacity={0.8}
        >
          STATIC BYPASS
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

        {/* HE mode: dotted line from inverter to output — tracking but not delivering */}
        {inverterTracking && (
          <line
            x1={nC.r} y1={nC.cy} x2={oC.l} y2={oC.cy}
            stroke={C.heStandby}
            strokeWidth="1.5"
            strokeDasharray="3 4"
            opacity="0.6"
          />
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
                 batteryDischarging ? C.battery :
                 batteryCharge < 30 ? C.fault :
                 batteryCharge < 60 ? C.bypass :
                 C.active}
        />

        {/* Efficiency badge (when available) */}
        {efficiency >= 0 && (
          <text
            x={(dC.r + nC.l) / 2} y={dC.t - 6}
            textAnchor="middle"
            fill={C.textDim}
            fontSize="8"
          >
            {(efficiency * 100 / 128).toFixed(0)}% eff
          </text>
        )}

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
 *   ┌───────┐                          ┌────────┐
 *   │ Input │─── Transfer Switch ─────►│ Output │
 *   └───────┘         │                └────────┘
 *                ┌────▼────┐
 *                │ Battery │
 *                └─────────┘
 *
 * On utility: input passes straight through to output.
 * On battery: transfer switch flips, battery → inverter → output.
 */

export function PowerFlowStandby({
  statusRaw: raw, inputVoltage, outputVoltage,
  batteryCharge, batteryVoltage, batteryError,
  loadPct,
}: PowerFlowProps) {

  const isOnline = !!(raw & ST.ONLINE)
  const isOnBattery = !!(raw & ST.ON_BATTERY)
  const isFault = !!(raw & (ST.FAULT | ST.FAULT_STATE))
  const isOff = !!(raw & ST.OUTPUT_OFF)
  const batFault = batteryError !== 0

  /* Path states */
  const utilityPath = isOnline && !isOnBattery && !isOff
  const batteryPath = isOnBattery && !isOff
  const batteryCharging = isOnline && !isOnBattery && !batFault && batteryCharge < 100

  /* Colors */
  const mainColor = isFault ? C.fault : C.active
  const batColor = isOnBattery ? C.battery : C.active

  /* Block borders */
  const inputBorder = isOnline ? mainColor : isFault ? C.fault : C.inactive
  const outBorder = (utilityPath || batteryPath) ? mainColor : isOff ? C.dead : C.inactive
  const switchBorder = (utilityPath || batteryPath) ? mainColor : C.inactive
  const batBorder = batFault ? C.fault : (batteryPath || batteryCharging) ? batColor : C.inactive

  /* Layout */
  const W = 460, H = 170
  const bw = 80, bh = 44

  const input    = { x: 10,  y: 20 }
  const xswitch  = { x: 190, y: 20 }
  const output   = { x: 370, y: 20 }
  const battery  = { x: 190, y: 110 }

  const cx = (b: {x: number; y: number}) => ({ l: b.x, r: b.x + bw, cx: b.x + bw/2, t: b.y, b: b.y + bh, cy: b.y + bh/2 })
  const iC = cx(input), sC = cx(xswitch), oC = cx(output), bC = cx(battery)

  const fmtV = (v: number) => v > 0 ? v.toFixed(1) : '--'
  const fmtPct = (v: number) => v >= 0 ? v.toFixed(0) : '--'

  return (
    <div className="rounded-lg bg-panel border border-edge p-4">
      <svg viewBox={`0 0 ${W} ${H}`} className="w-full" style={{ maxHeight: '190px' }}>

        {/* Input → Transfer Switch */}
        <FlowLine
          points={`${iC.r},${iC.cy} ${sC.l},${sC.cy}`}
          color={mainColor} active={utilityPath || batteryCharging}
        />

        {/* Transfer Switch → Output */}
        <FlowLine
          points={`${sC.r},${sC.cy} ${oC.l},${oC.cy}`}
          color={batteryPath ? batColor : mainColor}
          active={utilityPath || batteryPath}
        />

        {/* Transfer Switch ↔ Battery */}
        <FlowLine
          points={`${sC.cx},${sC.b} ${bC.cx},${bC.t}`}
          color={batColor}
          active={batteryPath || batteryCharging}
          reverse={batteryPath}
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

        <StageBlock {...battery} w={bw} h={bh} label="BATTERY"
          value={batFault ? 'Missing' : `${fmtPct(batteryCharge)}%`}
          borderColor={batBorder}
          color={batFault ? C.fault :
                 batteryPath ? C.battery :
                 batteryCharge < 30 ? C.fault :
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
