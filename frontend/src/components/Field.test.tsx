import { render, screen } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { describe, it, expect, vi } from 'vitest'
import { Field } from './Field'

describe('Field', () => {
  it('renders label and input with value', () => {
    render(<Field label="Voltage" value="120" onChange={() => {}} />)
    expect(screen.getByText('Voltage')).toBeInTheDocument()
    expect(screen.getByDisplayValue('120')).toBeInTheDocument()
  })

  it('calls onChange when user types', async () => {
    const onChange = vi.fn()
    render(<Field label="Name" value="" onChange={onChange} />)

    const input = screen.getByRole('textbox')
    await userEvent.type(input, 'a')
    expect(onChange).toHaveBeenCalledWith('a')
  })

  it('renders number input when type is number', () => {
    render(<Field label="Port" value="22" onChange={() => {}} type="number" />)
    expect(screen.getByRole('spinbutton')).toBeInTheDocument()
  })

  it('applies custom width class', () => {
    render(<Field label="X" value="" onChange={() => {}} width="w-20" />)
    const input = screen.getByRole('textbox')
    expect(input.className).toContain('w-20')
  })

  it('defaults to w-full when no width specified', () => {
    render(<Field label="X" value="" onChange={() => {}} />)
    const input = screen.getByRole('textbox')
    expect(input.className).toContain('w-full')
  })
})
