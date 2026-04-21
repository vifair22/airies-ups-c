import { render, screen } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { describe, it, expect, vi } from 'vitest'
import { Modal, WideModal, ConfirmModal } from './Modal'

describe('Modal', () => {
  it('renders children', () => {
    render(<Modal onClose={() => {}}><p>Hello modal</p></Modal>)
    expect(screen.getByText('Hello modal')).toBeInTheDocument()
  })

  it('calls onClose when backdrop is clicked', async () => {
    const onClose = vi.fn()
    render(<Modal onClose={onClose}><p>Content</p></Modal>)

    /* Click the backdrop (the outer fixed div) */
    await userEvent.click(screen.getByText('Content').parentElement!.parentElement!)
    expect(onClose).toHaveBeenCalledOnce()
  })

  it('does not call onClose when content is clicked', async () => {
    const onClose = vi.fn()
    render(<Modal onClose={onClose}><p>Content</p></Modal>)

    await userEvent.click(screen.getByText('Content'))
    expect(onClose).not.toHaveBeenCalled()
  })
})

describe('WideModal', () => {
  it('renders children', () => {
    render(<WideModal onClose={() => {}}><p>Wide content</p></WideModal>)
    expect(screen.getByText('Wide content')).toBeInTheDocument()
  })

  it('calls onClose on backdrop click', async () => {
    const onClose = vi.fn()
    render(<WideModal onClose={onClose}><p>Content</p></WideModal>)
    await userEvent.click(screen.getByText('Content').parentElement!.parentElement!)
    expect(onClose).toHaveBeenCalledOnce()
  })
})

describe('ConfirmModal', () => {
  const defaultProps = {
    title: 'Confirm Action',
    body: 'Are you sure?',
    confirmLabel: 'Yes',
    onConfirm: vi.fn(),
    onCancel: vi.fn(),
  }

  it('renders title, body, and buttons', () => {
    render(<ConfirmModal {...defaultProps} />)
    expect(screen.getByText('Confirm Action')).toBeInTheDocument()
    expect(screen.getByText('Are you sure?')).toBeInTheDocument()
    expect(screen.getByText('Yes')).toBeInTheDocument()
    expect(screen.getByText('Cancel')).toBeInTheDocument()
  })

  it('calls onConfirm when confirm button is clicked', async () => {
    const onConfirm = vi.fn()
    render(<ConfirmModal {...defaultProps} onConfirm={onConfirm} />)
    await userEvent.click(screen.getByText('Yes'))
    expect(onConfirm).toHaveBeenCalledOnce()
  })

  it('calls onCancel when cancel button is clicked', async () => {
    const onCancel = vi.fn()
    render(<ConfirmModal {...defaultProps} onCancel={onCancel} />)
    await userEvent.click(screen.getByText('Cancel'))
    expect(onCancel).toHaveBeenCalledOnce()
  })

  it('calls onCancel when backdrop is clicked', async () => {
    const onCancel = vi.fn()
    render(<ConfirmModal {...defaultProps} onCancel={onCancel} />)
    /* Click the backdrop */
    const backdrop = screen.getByText('Confirm Action').closest('.fixed')!
    await userEvent.click(backdrop)
    expect(onCancel).toHaveBeenCalled()
  })

  it('disables confirm button when loading', () => {
    render(<ConfirmModal {...defaultProps} loading />)
    expect(screen.getByText('...')).toBeDisabled()
  })

  it('applies danger styling when variant is danger', () => {
    render(<ConfirmModal {...defaultProps} confirmVariant="danger" />)
    const title = screen.getByText('Confirm Action')
    expect(title.className).toContain('text-red-400')
  })
})
