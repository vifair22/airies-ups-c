import { createContext, useCallback, useContext, useEffect, useRef, useState } from 'react'
import type { ReactNode } from 'react'
import { useApi } from './useApi'
import { useEventStream } from './useEventStream'
import type { UpsStatus } from '../types/ups'

/* --- Single shared SSE connection ---
 *
 * The previous design had each component (Layout topbar, Dashboard,
 * Commands, Events) calling useEventStream independently. That meant
 * every page mounted opened 2+ EventSource connections — same payload
 * arriving twice, two subscriber slots used per tab, two React state
 * updates per tick, real-felt slowness.
 *
 * This provider mounts inside AuthGuard (so the auth cookie is in
 * place before EventSource opens) and is the SOLE caller of
 * useEventStream in the app. Components read state via useSseState()
 * and subscribe to monitor events via useSseMonitor().
 *
 * Two separate contexts so monitor subscribers don't re-render on
 * every state tick — `subscribeMonitor` is stable across renders (it
 * just mutates a ref'd Set), so consumers of `useSseMonitor` don't
 * pay re-render cost when state changes. */

export type MonitorEvent = {
  severity: string
  category: string
  title: string
  message: string
}

type SubscribeMonitor = (cb: (e: MonitorEvent) => void) => () => void

const SseStateContext   = createContext<UpsStatus | null>(null)
const SseErrorContext   = createContext<string | null>(null)
const SseMonitorContext = createContext<SubscribeMonitor | null>(null)

export function SseProvider({ children }: { children: ReactNode }) {
  /* Initial /api/status for first paint (no polling). The broadcaster's
   * push-on-connect cache delivers the latest snapshot ~immediately
   * after subscribe, but before the first slow-loop tick the cache may
   * be empty — the buffered fetch covers that gap. */
  const { data: initial, error } = useApi<UpsStatus>('/api/status')
  const [live, setLive] = useState<UpsStatus | null>(null)

  const listenersRef = useRef<Set<(e: MonitorEvent) => void>>(new Set())

  useEventStream<{ state: UpsStatus; monitor: MonitorEvent }>('/api/events/stream', {
    state: setLive,
    monitor: (e) => {
      listenersRef.current.forEach((l) => {
        try { l(e) } catch { /* listener bug — don't take down the bus */ }
      })
    },
  })

  /* Seed live from the initial fetch only if SSE hasn't pushed anything
   * yet. After the first SSE state arrives, live wins and the initial
   * fetch becomes irrelevant. */
  useEffect(() => {
    if (initial && !live) setLive(initial)
  }, [initial, live])

  const subscribeMonitor = useCallback<SubscribeMonitor>((cb) => {
    listenersRef.current.add(cb)
    return () => { listenersRef.current.delete(cb) }
  }, [])

  return (
    <SseMonitorContext.Provider value={subscribeMonitor}>
      <SseErrorContext.Provider value={error ?? null}>
        <SseStateContext.Provider value={live ?? initial ?? null}>
          {children}
        </SseStateContext.Provider>
      </SseErrorContext.Provider>
    </SseMonitorContext.Provider>
  )
}

/* Latest UPS status snapshot. null until the first SSE state frame
 * arrives (or until the initial /api/status fetch lands). */
export function useSseState(): UpsStatus | null {
  return useContext(SseStateContext)
}

/* Initial-fetch error message, or null. Once SSE delivers a state frame
 * the live state takes over and consumers should typically ignore this. */
export function useSseError(): string | null {
  return useContext(SseErrorContext)
}

/* Subscribe to live monitor events for the lifetime of the calling
 * component. Each event invokes `cb` once. The callback can change
 * between renders without re-subscribing. */
export function useSseMonitor(cb: (e: MonitorEvent) => void): void {
  const subscribe = useContext(SseMonitorContext)
  const cbRef = useRef(cb)
  cbRef.current = cb

  useEffect(() => {
    if (!subscribe) return
    return subscribe((e) => cbRef.current(e))
  }, [subscribe])
}
