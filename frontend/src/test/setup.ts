import '@testing-library/jest-dom/vitest'
import { cleanup } from '@testing-library/react'
import { afterEach, vi } from 'vitest'

/* Default matchMedia mock for jsdom (doesn't support it natively) */
Object.defineProperty(window, 'matchMedia', {
  writable: true,
  value: vi.fn().mockImplementation((query: string) => ({
    matches: query === '(prefers-color-scheme: dark)',
    media: query,
    addEventListener: vi.fn(),
    removeEventListener: vi.fn(),
    addListener: vi.fn(),
    removeListener: vi.fn(),
    dispatchEvent: vi.fn(),
  })),
})

/* jsdom doesn't ship EventSource. Provide an inert default so pages
 * using useEventStream can render in tests without crashing — tests that
 * need to drive the stream install their own controllable replacement
 * in beforeEach (see useEventStream.test.ts). */
class NoopEventSource {
  static CONNECTING = 0
  static OPEN = 1
  static CLOSED = 2
  readonly url: string
  readonly withCredentials: boolean
  readyState = 1
  constructor(url: string, init?: { withCredentials?: boolean }) {
    this.url = url
    this.withCredentials = init?.withCredentials ?? false
  }
  addEventListener(): void {}
  removeEventListener(): void {}
  close(): void {}
}
if (typeof (globalThis as { EventSource?: unknown }).EventSource === 'undefined') {
  ;(globalThis as unknown as { EventSource: typeof NoopEventSource }).EventSource = NoopEventSource
}

const originalFetch = globalThis.fetch

afterEach(() => {
  cleanup()
  localStorage.clear()
  globalThis.fetch = originalFetch
})
