import { render, screen, act } from '@testing-library/react'
import { describe, it, expect, vi, beforeEach } from 'vitest'
import { useToast, ToastContainer } from './Toast'

/* Test wrapper that exposes the hook via buttons */
function TestHarness() {
  const { toasts, push } = useToast()
  return (
    <div>
      <button onClick={() => push('Success!', 'success')}>Push Success</button>
      <button onClick={() => push('Error!', 'error')}>Push Error</button>
      <ToastContainer toasts={toasts} />
    </div>
  )
}

beforeEach(() => {
  vi.useFakeTimers()
})

describe('Toast system', () => {
  it('renders nothing when there are no toasts', () => {
    const { container } = render(<ToastContainer toasts={[]} />)
    expect(container.innerHTML).toBe('')
  })

  it('shows a success toast when pushed', async () => {
    render(<TestHarness />)

    await act(async () => {
      screen.getByText('Push Success').click()
    })
    expect(screen.getByText('Success!')).toBeInTheDocument()
  })

  it('shows an error toast when pushed', async () => {
    render(<TestHarness />)

    await act(async () => {
      screen.getByText('Push Error').click()
    })
    const toast = screen.getByText('Error!')
    expect(toast).toBeInTheDocument()
    expect(toast.className).toContain('bg-red-600')
  })

  it('auto-dismisses toasts after 4 seconds', async () => {
    render(<TestHarness />)

    await act(async () => {
      screen.getByText('Push Success').click()
    })
    expect(screen.getByText('Success!')).toBeInTheDocument()

    await act(async () => {
      vi.advanceTimersByTime(4100)
    })
    expect(screen.queryByText('Success!')).not.toBeInTheDocument()
  })

  it('can show multiple toasts simultaneously', async () => {
    render(<TestHarness />)

    await act(async () => {
      screen.getByText('Push Success').click()
      screen.getByText('Push Error').click()
    })

    expect(screen.getByText('Success!')).toBeInTheDocument()
    expect(screen.getByText('Error!')).toBeInTheDocument()
  })
})
