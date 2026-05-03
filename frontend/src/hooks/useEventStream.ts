import { useEffect, useRef } from 'react'

/* --- Live event stream subscription (Server-Sent Events) ---
 *
 * Subscribes to a multiplexed SSE endpoint (one connection, multiple
 * `event:` types). The cookie auth set by /api/auth/login rides along
 * automatically because EventSource sends credentials when
 * `withCredentials: true` is set.
 *
 * Handlers are stable across renders: the hook stores them in a ref and
 * the underlying EventSource is reopened only when the URL changes. Pass
 * a `handlers` object mapping each event type to a callback that takes
 * the parsed JSON payload. Malformed payloads are dropped silently
 * rather than killing the page. */

export type EventStreamHandlers<T extends Record<string, unknown>> = {
  [K in keyof T]?: (event: T[K]) => void
}

export function useEventStream<T extends Record<string, unknown>>(
  url: string,
  handlers: EventStreamHandlers<T>
): void {
  const handlersRef = useRef(handlers)
  handlersRef.current = handlers

  useEffect(() => {
    const es = new EventSource(url, { withCredentials: true })
    const eventTypes = Object.keys(handlersRef.current)
    const wired: Array<[string, EventListener]> = []

    for (const type of eventTypes) {
      const listener: EventListener = (msg) => {
        const cur = handlersRef.current[type as keyof T]
        if (!cur) return
        const data = (msg as MessageEvent).data
        try {
          cur(JSON.parse(data) as T[keyof T])
        } catch {
          /* malformed payload — drop */
        }
      }
      es.addEventListener(type, listener)
      wired.push([type, listener])
    }

    return () => {
      for (const [type, listener] of wired)
        es.removeEventListener(type, listener)
      es.close()
    }
  }, [url])
}
