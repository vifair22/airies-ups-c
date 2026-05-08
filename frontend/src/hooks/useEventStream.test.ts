import { renderHook } from '@testing-library/react'
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest'
import { useEventStream } from './useEventStream'

/* --- MockEventSource ---
 *
 * jsdom doesn't ship EventSource. We install a controllable mock on the
 * global so the hook can construct it like normal, and the test can
 * dispatch events / inspect close() calls. */

class MockEventSource {
  readonly url: string
  readonly withCredentials: boolean
  listeners: Map<string, Set<EventListener>> = new Map()
  closed = false

  static instances: MockEventSource[] = []

  constructor(url: string, init?: { withCredentials?: boolean }) {
    this.url = url
    this.withCredentials = init?.withCredentials ?? false
    MockEventSource.instances.push(this)
  }

  addEventListener(type: string, listener: EventListener): void {
    if (!this.listeners.has(type)) this.listeners.set(type, new Set())
    this.listeners.get(type)!.add(listener)
  }

  removeEventListener(type: string, listener: EventListener): void {
    this.listeners.get(type)?.delete(listener)
  }

  close(): void {
    this.closed = true
  }

  /* Helper: deliver a synthetic SSE message to listeners for `type`. */
  dispatch(type: string, data: string): void {
    const ev = { type, data } as MessageEvent
    this.listeners.get(type)?.forEach((l) => l(ev))
  }
}

beforeEach(() => {
  MockEventSource.instances = []
  ;(globalThis as unknown as { EventSource: typeof MockEventSource }).EventSource = MockEventSource
})

afterEach(() => {
  MockEventSource.instances = []
})

describe('useEventStream', () => {
  it('opens an EventSource at the given URL with cookies', () => {
    renderHook(() => useEventStream('/api/events/stream', { monitor: vi.fn() }))
    expect(MockEventSource.instances).toHaveLength(1)
    expect(MockEventSource.instances[0]!.url).toBe('/api/events/stream')
    expect(MockEventSource.instances[0]!.withCredentials).toBe(true)
  })

  it('invokes the handler with parsed JSON payloads', () => {
    const onMonitor = vi.fn()
    renderHook(() => useEventStream('/api/events/stream', { monitor: onMonitor }))
    const es = MockEventSource.instances[0]!
    es.dispatch('monitor', '{"severity":"warning","title":"On Battery"}')
    expect(onMonitor).toHaveBeenCalledWith({
      severity: 'warning',
      title: 'On Battery',
    })
  })

  it('routes each event type to its own handler with no cross-talk', () => {
    const onMonitor = vi.fn()
    const onState = vi.fn()
    renderHook(() =>
      useEventStream('/api/events/stream', { monitor: onMonitor, state: onState })
    )
    const es = MockEventSource.instances[0]!
    es.dispatch('monitor', '{"k":1}')
    es.dispatch('state', '{"v":42}')

    expect(onMonitor).toHaveBeenCalledTimes(1)
    expect(onMonitor).toHaveBeenCalledWith({ k: 1 })
    expect(onState).toHaveBeenCalledTimes(1)
    expect(onState).toHaveBeenCalledWith({ v: 42 })
  })

  it('drops malformed JSON without crashing the handler', () => {
    const onMonitor = vi.fn()
    renderHook(() => useEventStream('/api/events/stream', { monitor: onMonitor }))
    const es = MockEventSource.instances[0]!
    /* No exception even though payload is not valid JSON. */
    expect(() => es.dispatch('monitor', 'not json')).not.toThrow()
    expect(onMonitor).not.toHaveBeenCalled()
  })

  it('closes the EventSource on unmount', () => {
    const { unmount } = renderHook(() =>
      useEventStream('/api/events/stream', { monitor: vi.fn() })
    )
    const es = MockEventSource.instances[0]!
    expect(es.closed).toBe(false)
    unmount()
    expect(es.closed).toBe(true)
  })

  it('keeps a single connection across re-renders that change handlers', () => {
    let calls = 0
    const { rerender } = renderHook(
      ({ handler }: { handler: () => void }) =>
        useEventStream('/api/events/stream', { monitor: handler }),
      { initialProps: { handler: () => calls++ } }
    )
    expect(MockEventSource.instances).toHaveLength(1)
    /* Pass a new handler; same URL, so connection should stay open. */
    rerender({ handler: () => (calls += 10) })
    expect(MockEventSource.instances).toHaveLength(1)

    const es = MockEventSource.instances[0]!
    es.dispatch('monitor', '{}')
    /* The latest handler runs, not the original. */
    expect(calls).toBe(10)
  })
})
