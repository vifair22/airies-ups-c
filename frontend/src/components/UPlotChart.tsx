import { useEffect, useRef } from 'react'
import uPlot from 'uplot'

interface Props {
  opts: Omit<uPlot.Options, 'width' | 'height'>
  data: uPlot.AlignedData
  height: number
  className?: string
}

/* Thin React wrapper around uPlot.
 *
 * Creates the plot on mount and destroys it on unmount. Container width
 * is driven by a ResizeObserver so charts adapt to layout changes. Data
 * updates go through setData (fast path); opts or height changes recreate
 * the plot, so callers should memoize opts via useMemo to avoid needless
 * re-creation on every render. */
export function UPlotChart({ opts, data, height, className }: Props) {
  const containerRef = useRef<HTMLDivElement>(null)
  const plotRef = useRef<uPlot | null>(null)

  useEffect(() => {
    const el = containerRef.current
    if (!el) return

    const width = el.clientWidth || 400
    const plot = new uPlot({ ...opts, width, height }, data, el)
    plotRef.current = plot

    const ro = new ResizeObserver(() => {
      const w = el.clientWidth
      if (w > 0) plot.setSize({ width: w, height })
    })
    ro.observe(el)

    return () => {
      ro.disconnect()
      plot.destroy()
      plotRef.current = null
    }
    /* data intentionally omitted — handled by the setData effect below so
     * refreshes don't destroy and recreate the canvas. */
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [opts, height])

  useEffect(() => {
    plotRef.current?.setData(data)
  }, [data])

  return <div ref={containerRef} className={className} />
}
