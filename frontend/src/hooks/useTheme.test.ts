import { renderHook, act } from '@testing-library/react'
import { describe, it, expect, vi, beforeEach } from 'vitest'
import { useTheme } from './useTheme'

let matchMediaListeners: Array<() => void> = []

beforeEach(() => {
  matchMediaListeners = []
  document.documentElement.removeAttribute('data-theme')

  /* Mock matchMedia — defaults to dark preference */
  Object.defineProperty(window, 'matchMedia', {
    writable: true,
    value: vi.fn().mockImplementation((query: string) => ({
      matches: query === '(prefers-color-scheme: dark)',
      media: query,
      addEventListener: (_: string, handler: () => void) => { matchMediaListeners.push(handler) },
      removeEventListener: vi.fn(),
    })),
  })
})

describe('useTheme', () => {
  it('defaults to auto mode when nothing is stored', () => {
    const { result } = renderHook(() => useTheme())
    expect(result.current.mode).toBe('auto')
  })

  it('reads stored mode from localStorage', () => {
    localStorage.setItem('theme', 'light')
    const { result } = renderHook(() => useTheme())
    expect(result.current.mode).toBe('light')
  })

  it('ignores invalid stored values', () => {
    localStorage.setItem('theme', 'neon')
    const { result } = renderHook(() => useTheme())
    expect(result.current.mode).toBe('auto')
  })

  it('applies light theme by setting data-theme attribute', () => {
    const { result } = renderHook(() => useTheme())
    act(() => result.current.setMode('light'))

    expect(result.current.mode).toBe('light')
    expect(document.documentElement.getAttribute('data-theme')).toBe('light')
    expect(localStorage.getItem('theme')).toBe('light')
  })

  it('applies dark theme by removing data-theme attribute', () => {
    document.documentElement.setAttribute('data-theme', 'light')
    const { result } = renderHook(() => useTheme())
    act(() => result.current.setMode('dark'))

    expect(result.current.mode).toBe('dark')
    expect(document.documentElement.getAttribute('data-theme')).toBeNull()
  })

  it('auto mode resolves to system preference (dark)', () => {
    const { result } = renderHook(() => useTheme())
    act(() => result.current.setMode('auto'))

    /* matchMedia mocked to prefer dark */
    expect(document.documentElement.getAttribute('data-theme')).toBeNull()
  })

  it('auto mode resolves to light when system prefers light', () => {
    Object.defineProperty(window, 'matchMedia', {
      writable: true,
      value: vi.fn().mockImplementation((query: string) => ({
        matches: query === '(prefers-color-scheme: light)',
        media: query,
        addEventListener: vi.fn(),
        removeEventListener: vi.fn(),
      })),
    })

    const { result } = renderHook(() => useTheme())
    act(() => result.current.setMode('auto'))

    expect(document.documentElement.getAttribute('data-theme')).toBe('light')
  })

  it('persists mode to localStorage on change', () => {
    const { result } = renderHook(() => useTheme())

    act(() => result.current.setMode('dark'))
    expect(localStorage.getItem('theme')).toBe('dark')

    act(() => result.current.setMode('auto'))
    expect(localStorage.getItem('theme')).toBe('auto')
  })

  it('reacts to system preference changes in auto mode', () => {
    const { result } = renderHook(() => useTheme())

    /* Start in auto mode (dark system pref → no data-theme) */
    expect(result.current.mode).toBe('auto')
    expect(document.documentElement.getAttribute('data-theme')).toBeNull()

    /* Simulate system preference change */
    expect(matchMediaListeners.length).toBeGreaterThan(0)

    /* Switch matchMedia to prefer light, then fire the listener */
    Object.defineProperty(window, 'matchMedia', {
      writable: true,
      value: vi.fn().mockImplementation((query: string) => ({
        matches: query === '(prefers-color-scheme: light)',
        media: query,
        addEventListener: vi.fn(),
        removeEventListener: vi.fn(),
      })),
    })

    act(() => { matchMediaListeners.forEach(fn => fn()) })
    expect(document.documentElement.getAttribute('data-theme')).toBe('light')
  })
})
