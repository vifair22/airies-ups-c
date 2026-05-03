/* Shared test utilities */

import { MemoryRouter } from 'react-router-dom'
import { render } from '@testing-library/react'
import { vi } from 'vitest'
import type { ReactElement } from 'react'
import { SseProvider } from '../hooks/SseProvider'

/* Render inside a MemoryRouter and the SseProvider that wraps the
 * protected routes in the real app. Tests that mock /api/status via
 * mockApiResponses get that data routed through the provider's internal
 * useApi exactly like in production. The NoopEventSource installed in
 * test/setup.ts keeps the provider's useEventStream from blowing up. */
export function renderWithRouter(ui: ReactElement, { route = '/' } = {}) {
  return render(
    <MemoryRouter initialEntries={[route]}>
      <SseProvider>
        {ui}
      </SseProvider>
    </MemoryRouter>
  )
}

/* Mock fetch to return a sequence of responses keyed by URL substring */
export function mockApiResponses(responses: Record<string, unknown>) {
  globalThis.fetch = vi.fn().mockImplementation((url: string) => {
    for (const [pattern, body] of Object.entries(responses)) {
      if (url.includes(pattern)) {
        return Promise.resolve({
          ok: true,
          status: 200,
          statusText: 'OK',
          json: () => Promise.resolve(body),
        })
      }
    }
    return Promise.resolve({
      ok: false,
      status: 404,
      statusText: 'Not Found',
      json: () => Promise.resolve({}),
    })
  })
}
