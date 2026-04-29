import { render, screen } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { describe, it, expect, vi } from 'vitest'
import { SecretField } from './SecretField'

describe('SecretField', () => {
  it('renders single-line as masked input', () => {
    render(<SecretField label="Password" value="hunter2" onChange={() => {}} />)
    expect(screen.getByText('Password')).toBeInTheDocument()
    const input = screen.getByDisplayValue('hunter2') as HTMLInputElement
    expect(input.tagName).toBe('INPUT')
    expect(input.type).toBe('password')
  })

  it('renders multiline as masked textarea', () => {
    render(<SecretField label="Key" multiline value="-----BEGIN-----" onChange={() => {}} />)
    const ta = screen.getByDisplayValue('-----BEGIN-----') as HTMLTextAreaElement
    expect(ta.tagName).toBe('TEXTAREA')
    /* JSDom drops the non-standard -webkit-text-security style, so we
     * assert on the data-masked attribute instead. */
    expect(ta.dataset.masked).toBe('true')
  })

  it('show/hide toggle flips single-line type between password and text', async () => {
    render(<SecretField label="Password" value="hunter2" onChange={() => {}} />)

    const toggle = screen.getByRole('button', { name: 'show' })
    let input = screen.getByDisplayValue('hunter2') as HTMLInputElement
    expect(input.type).toBe('password')

    await userEvent.click(toggle)
    input = screen.getByDisplayValue('hunter2') as HTMLInputElement
    expect(input.type).toBe('text')
    expect(screen.getByRole('button', { name: 'hide' })).toBeInTheDocument()

    await userEvent.click(screen.getByRole('button', { name: 'hide' }))
    input = screen.getByDisplayValue('hunter2') as HTMLInputElement
    expect(input.type).toBe('password')
  })

  it('show/hide toggle flips data-masked on multiline', async () => {
    render(<SecretField label="Key" multiline value="secret" onChange={() => {}} />)

    let ta = screen.getByDisplayValue('secret') as HTMLTextAreaElement
    expect(ta.dataset.masked).toBe('true')

    await userEvent.click(screen.getByRole('button', { name: 'show' }))
    ta = screen.getByDisplayValue('secret') as HTMLTextAreaElement
    expect(ta.dataset.masked).toBe('false')
  })

  it('calls onChange when typing', async () => {
    const onChange = vi.fn()
    const { container } = render(<SecretField label="Password" value="" onChange={onChange} />)
    /* type=password has no implicit ARIA role and there's no htmlFor,
     * so direct query is the cleanest path. */
    const input = container.querySelector('input') as HTMLInputElement
    await userEvent.type(input, 'a')
    expect(onChange).toHaveBeenCalledWith('a')
  })

  it('passes placeholder through', () => {
    render(<SecretField label="Pw" value="" onChange={() => {}} placeholder="leave blank to keep existing" />)
    expect(screen.getByPlaceholderText('leave blank to keep existing')).toBeInTheDocument()
  })
})
