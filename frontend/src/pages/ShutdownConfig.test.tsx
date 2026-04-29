import { screen, waitFor } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { describe, it, expect, vi, beforeEach } from 'vitest'
import { renderWithRouter, mockApiResponses } from '../test/helpers'
import ShutdownConfig from './ShutdownConfig'
import type { ShutdownGroup, ShutdownTarget, ShutdownSettings } from '../types/shutdown'

beforeEach(() => {
  vi.restoreAllMocks()
  localStorage.setItem('auth_token', 'test-jwt')
})

const mockGroups: ShutdownGroup[] = [
  { id: 1, name: 'Servers', execution_order: 0, parallel: true, max_timeout_sec: 300, post_group_delay: 10 },
  { id: 2, name: 'Network', execution_order: 1, parallel: false, max_timeout_sec: 0, post_group_delay: 0 },
]

const mockTargets: ShutdownTarget[] = [
  {
    id: 1, name: 'web-server', method: 'ssh_key', host: '10.0.0.10', username: 'root',
    command: 'poweroff', timeout_sec: 180, order_in_group: 0, group: 'Servers', group_id: 1,
    confirm_method: 'ping', confirm_port: 22, confirm_command: '', post_confirm_delay: 15,
  },
  {
    id: 2, name: 'db-server', method: 'ssh_key', host: '10.0.0.11', username: 'root',
    command: 'poweroff', timeout_sec: 180, order_in_group: 1, group: 'Servers', group_id: 1,
    confirm_method: 'tcp_port', confirm_port: 5432, confirm_command: '', post_confirm_delay: 10,
  },
]

const mockSettings: ShutdownSettings = {
  trigger: {
    mode: 'software', source: 'runtime', runtime_sec: 300, battery_pct: 0,
    on_battery: true, delay_sec: 30, field: '', field_op: 'lt', field_value: 0,
  },
  ups_action: { mode: 'command', register: '', value: 0, delay: 5 },
  controller: { enabled: true },
}

function mockAll(overrides?: {
  groups?: ShutdownGroup[]
  targets?: ShutdownTarget[]
  settings?: ShutdownSettings
}) {
  mockApiResponses({
    '/api/shutdown/groups': overrides?.groups ?? mockGroups,
    '/api/shutdown/targets': overrides?.targets ?? mockTargets,
    '/api/shutdown/settings': overrides?.settings ?? mockSettings,
  })
}

/* ══════════════════════════════════════════════════════
 *  Trigger Settings
 * ══════════════════════════════════════════════════════ */

describe('ShutdownConfig — Trigger', () => {
  it('renders trigger section with software mode selected', async () => {
    mockAll()
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getByText('Trigger')).toBeInTheDocument()
    })
    expect(screen.getByDisplayValue('Automatic - Software')).toBeInTheDocument()
  })

  it('shows runtime/battery fields in software runtime mode', async () => {
    mockAll()
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getByText('Runtime Below (s)')).toBeInTheDocument()
    })
    expect(screen.getByText('Battery Below (%)')).toBeInTheDocument()
    expect(screen.getByText('Debounce (s)')).toBeInTheDocument()
  })

  it('switches to data field source', async () => {
    mockAll()
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getByText('Runtime / Battery')).toBeInTheDocument()
    })

    await userEvent.click(screen.getByLabelText('Data Field'))

    expect(screen.getByText('Field')).toBeInTheDocument()
    expect(screen.getByText('Operator')).toBeInTheDocument()
  })

  it('shows UPS mode description when UPS trigger selected', async () => {
    mockAll({
      settings: {
        ...mockSettings,
        trigger: { ...mockSettings.trigger, mode: 'ups' },
      },
    })
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getByText(/shutdown-imminent flag/)).toBeInTheDocument()
    })
  })

  it('shows manual mode description', async () => {
    mockAll({
      settings: {
        ...mockSettings,
        trigger: { ...mockSettings.trigger, mode: 'manual' },
      },
    })
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getByText(/manually triggered/)).toBeInTheDocument()
    })
  })

  it('shows Save Settings button when settings are changed', async () => {
    mockAll()
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getByDisplayValue('Automatic - Software')).toBeInTheDocument()
    })

    /* Change trigger mode to manual */
    await userEvent.selectOptions(screen.getByDisplayValue('Automatic - Software'), 'manual')
    expect(screen.getByText('Save Settings')).toBeInTheDocument()
  })
})

/* ══════════════════════════════════════════════════════
 *  Group Display
 * ══════════════════════════════════════════════════════ */

describe('ShutdownConfig — Groups', () => {
  it('renders group cards with names', async () => {
    mockAll()
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getByText('Servers')).toBeInTheDocument()
    })
    expect(screen.getByText('Network')).toBeInTheDocument()
  })

  it('shows parallel/sequential badge', async () => {
    mockAll()
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getByText('parallel')).toBeInTheDocument()
    })
    expect(screen.getByText('sequential')).toBeInTheDocument()
  })

  it('shows max timeout when set', async () => {
    mockAll()
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getByText('max 300s')).toBeInTheDocument()
    })
  })

  it('shows post-group delay when set', async () => {
    mockAll()
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getByText('+10s delay')).toBeInTheDocument()
    })
  })

  it('shows Add Group button', async () => {
    mockAll()
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getByText('+ Add Group')).toBeInTheDocument()
    })
  })

  it('opens add group form', async () => {
    mockAll()
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getByText('+ Add Group')).toBeInTheDocument()
    })

    await userEvent.click(screen.getByText('+ Add Group'))
    expect(screen.getByText('New Group')).toBeInTheDocument()
    expect(screen.getByText('Create')).toBeInTheDocument()
  })

  it('opens group edit form', async () => {
    mockAll()
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getByText('Servers')).toBeInTheDocument()
    })

    /* Click Edit on the first group */
    const editButtons = screen.getAllByText('Edit')
    await userEvent.click(editButtons[0])

    /* Should show the edit form with Save/Cancel */
    expect(screen.getByText('Save')).toBeInTheDocument()
  })
})

/* ══════════════════════════════════════════════════════
 *  Targets
 * ══════════════════════════════════════════════════════ */

describe('ShutdownConfig — Targets', () => {
  it('renders targets within their groups', async () => {
    mockAll()
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getByText('web-server')).toBeInTheDocument()
    })
    expect(screen.getByText('db-server')).toBeInTheDocument()
    expect(screen.getByText('10.0.0.10')).toBeInTheDocument()
    expect(screen.getByText('10.0.0.11')).toBeInTheDocument()
  })

  it('shows target method and command', async () => {
    mockAll()
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getAllByText('ssh_key').length).toBe(2)
    })
    expect(screen.getAllByText('poweroff').length).toBe(2)
  })

  it('shows confirmation method and timeout', async () => {
    mockAll()
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getByText('ping / 180s')).toBeInTheDocument()
    })
    expect(screen.getByText('tcp_port / 180s')).toBeInTheDocument()
  })

  it('shows empty state for group with no targets', async () => {
    mockAll({ targets: [] })
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getAllByText('No targets in this group.').length).toBe(2)
    })
  })

  it('shows Add Target button for each group', async () => {
    mockAll()
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getAllByText('+ Add Target').length).toBe(2)
    })
  })

  it('opens target add form', async () => {
    mockAll()
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getAllByText('+ Add Target').length).toBe(2)
    })

    await userEvent.click(screen.getAllByText('+ Add Target')[0])
    expect(screen.getByText('Add Target')).toBeInTheDocument()
    expect(screen.getByText('Action Method')).toBeInTheDocument()
    expect(screen.getByText('Confirmation')).toBeInTheDocument()
  })

  it('opens target edit form', async () => {
    mockAll()
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getByText('web-server')).toBeInTheDocument()
    })

    /* Click Edit on first target */
    const editButtons = screen.getAllByText('Edit')
    /* First Edit is the group edit, second is the target */
    await userEvent.click(editButtons[1])

    /* Target form should appear with Save button */
    expect(screen.getByRole('button', { name: 'Save' })).toBeInTheDocument()
  })

  it('confirmation method swaps form fields', async () => {
    mockAll()
    const { container } = renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getAllByText('+ Add Target').length).toBe(2)
    })

    await userEvent.click(screen.getAllByText('+ Add Target')[0])

    /* default confirm_method='ping' — no port field */
    expect(screen.queryByText('Port')).not.toBeInTheDocument()
    expect(screen.queryByText('Confirm Command')).not.toBeInTheDocument()

    const confirmSelect = container.querySelector('select[value="ping"]') as HTMLSelectElement
        ?? Array.from(container.querySelectorAll('select')).find(s =>
             Array.from(s.options).some(o => o.value === 'tcp_port'))!

    /* tcp_port → Port number input appears */
    await userEvent.selectOptions(confirmSelect, 'tcp_port')
    expect(screen.getByText('Port')).toBeInTheDocument()

    /* command → Confirm Command text input appears */
    await userEvent.selectOptions(confirmSelect, 'command')
    expect(screen.getByText('Confirm Command')).toBeInTheDocument()
    expect(screen.queryByText('Port')).not.toBeInTheDocument()

    /* none → both gone */
    await userEvent.selectOptions(confirmSelect, 'none')
    expect(screen.queryByText('Port')).not.toBeInTheDocument()
    expect(screen.queryByText('Confirm Command')).not.toBeInTheDocument()
  })

  it('group edit save fires PUT', async () => {
    globalThis.fetch = vi.fn().mockImplementation((url: string, opts?: RequestInit) => {
      if (opts?.method === 'PUT' || opts?.method === 'POST' || opts?.method === 'DELETE') {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve({ ok: true }) })
      }
      if (url.includes('/api/shutdown/groups')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(mockGroups) })
      }
      if (url.includes('/api/shutdown/targets')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(mockTargets) })
      }
      if (url.includes('/api/shutdown/settings')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(mockSettings) })
      }
      return Promise.resolve({ ok: false, status: 404, json: () => Promise.resolve({}) })
    })

    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getByText('Servers')).toBeInTheDocument()
    })

    /* First Edit button is the group edit (group header comes before targets) */
    await userEvent.click(screen.getAllByText('Edit')[0])

    /* Save without changes — exercises updateGroup */
    await userEvent.click(screen.getByRole('button', { name: 'Save' }))

    await waitFor(() => {
      const calls = (globalThis.fetch as ReturnType<typeof vi.fn>).mock.calls
      const put = calls.find((call: unknown[]) => {
        const [url, optsAny] = call as [string, RequestInit?]
        return url.includes('/api/shutdown/groups') && optsAny?.method === 'PUT'
      })
      expect(put).toBeDefined()
    })
  })

  it('group delete fires DELETE after confirm', async () => {
    vi.spyOn(window, 'confirm').mockReturnValue(true)
    globalThis.fetch = vi.fn().mockImplementation((url: string, opts?: RequestInit) => {
      if (opts?.method === 'DELETE') {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve({ ok: true }) })
      }
      if (url.includes('/api/shutdown/groups')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(mockGroups) })
      }
      if (url.includes('/api/shutdown/targets')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(mockTargets) })
      }
      if (url.includes('/api/shutdown/settings')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(mockSettings) })
      }
      return Promise.resolve({ ok: false, status: 404, json: () => Promise.resolve({}) })
    })

    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getByText('Servers')).toBeInTheDocument()
    })

    /* Click first Delete (group-level) — there are multiple Delete buttons */
    await userEvent.click(screen.getAllByText('Delete')[0])

    await waitFor(() => {
      const calls = (globalThis.fetch as ReturnType<typeof vi.fn>).mock.calls
      const del = calls.find((call: unknown[]) => {
        const [url, optsAny] = call as [string, RequestInit?]
        return url.includes('/api/shutdown/groups') && optsAny?.method === 'DELETE'
      })
      expect(del).toBeDefined()
    })
  })

  it('target delete fires DELETE after confirm', async () => {
    vi.spyOn(window, 'confirm').mockReturnValue(true)
    globalThis.fetch = vi.fn().mockImplementation((url: string, opts?: RequestInit) => {
      if (opts?.method === 'DELETE') {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve({ ok: true }) })
      }
      if (url.includes('/api/shutdown/groups')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(mockGroups) })
      }
      if (url.includes('/api/shutdown/targets')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(mockTargets) })
      }
      if (url.includes('/api/shutdown/settings')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(mockSettings) })
      }
      return Promise.resolve({ ok: false, status: 404, json: () => Promise.resolve({}) })
    })

    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getByText('web-server')).toBeInTheDocument()
    })

    /* Second Delete button is the first target's */
    await userEvent.click(screen.getAllByText('Delete')[1])

    await waitFor(() => {
      const calls = (globalThis.fetch as ReturnType<typeof vi.fn>).mock.calls
      const del = calls.find((call: unknown[]) => {
        const [url, optsAny] = call as [string, RequestInit?]
        return url.includes('/api/shutdown/targets') && optsAny?.method === 'DELETE'
      })
      expect(del).toBeDefined()
    })
  })

  it('updates a target via PUT', async () => {
    /* Inline mockCrud so PUTs return ok */
    globalThis.fetch = vi.fn().mockImplementation((url: string, opts?: RequestInit) => {
      if (opts?.method === 'POST' || opts?.method === 'PUT' || opts?.method === 'DELETE') {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve({ ok: true }) })
      }
      if (url.includes('/api/shutdown/groups')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(mockGroups) })
      }
      if (url.includes('/api/shutdown/targets')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(mockTargets) })
      }
      if (url.includes('/api/shutdown/settings')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(mockSettings) })
      }
      return Promise.resolve({ ok: false, status: 404, json: () => Promise.resolve({}) })
    })

    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getByText('web-server')).toBeInTheDocument()
    })

    /* Click Edit on the first target (second Edit button — first is group) */
    const editButtons = screen.getAllByText('Edit')
    await userEvent.click(editButtons[1])

    /* TargetEditRow opened */
    expect(screen.getByRole('button', { name: 'Save' })).toBeInTheDocument()

    /* Save without changes — preserves credential, exercises updateTarget */
    await userEvent.click(screen.getByRole('button', { name: 'Save' }))

    await waitFor(() => {
      const calls = (globalThis.fetch as ReturnType<typeof vi.fn>).mock.calls
      const put = calls.find((call: unknown[]) => {
        const [url, optsAny] = call as [string, RequestInit?]
        return url.includes('/api/shutdown/targets') && optsAny?.method === 'PUT'
      })
      expect(put).toBeDefined()
    })
  })

  it('credential widget swaps with method', async () => {
    mockAll()
    const { container } = renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getAllByText('+ Add Target').length).toBe(2)
    })

    await userEvent.click(screen.getAllByText('+ Add Target')[0])

    /* EMPTY_TARGET defaults to method='ssh_key' → multiline textarea visible */
    expect(screen.getByText('Private Key (PEM)')).toBeInTheDocument()
    expect(container.querySelector('textarea')).toBeTruthy()

    /* Switch to ssh_password → single-line input, label changes */
    const methodSelect = screen.getByDisplayValue('SSH Key')
    await userEvent.selectOptions(methodSelect, 'ssh_password')
    expect(screen.getByText('Password')).toBeInTheDocument()
    expect(container.querySelector('textarea')).toBeFalsy()

    /* Switch to command → no credential field at all */
    await userEvent.selectOptions(screen.getByDisplayValue('SSH Password'), 'command')
    expect(screen.queryByText('Password')).not.toBeInTheDocument()
    expect(screen.queryByText('Private Key (PEM)')).not.toBeInTheDocument()
  })
})

/* ══════════════════════════════════════════════════════
 *  UPS Action
 * ══════════════════════════════════════════════════════ */

describe('ShutdownConfig — UPS Action', () => {
  it('renders UPS action section', async () => {
    mockAll()
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getByText('UPS Action')).toBeInTheDocument()
    })
    expect(screen.getByLabelText('Send Shutdown Command')).toBeChecked()
  })

  it('shows command description when command mode selected', async () => {
    mockAll()
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getByText(/shutdown command to the UPS/)).toBeInTheDocument()
    })
  })

  it('shows register fields when register mode selected', async () => {
    mockAll({
      settings: {
        ...mockSettings,
        ups_action: { mode: 'register', register: 'ups_shutdown', value: 1, delay: 5 },
      },
    })
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getByText('Register')).toBeInTheDocument()
    })
  })

  it('shows none description when none selected', async () => {
    mockAll({
      settings: {
        ...mockSettings,
        ups_action: { mode: 'none', register: '', value: 0, delay: 0 },
      },
    })
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getByText(/will not be commanded/)).toBeInTheDocument()
    })
  })
})

/* ══════════════════════════════════════════════════════
 *  Controller
 * ══════════════════════════════════════════════════════ */

describe('ShutdownConfig — Controller', () => {
  it('renders controller section with checkbox', async () => {
    mockAll()
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getByText('Controller')).toBeInTheDocument()
    })
    expect(screen.getByLabelText('Shut down this controller after all other steps complete')).toBeChecked()
  })

  it('unchecking controller marks settings dirty', async () => {
    mockAll()
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getByLabelText('Shut down this controller after all other steps complete')).toBeInTheDocument()
    })

    await userEvent.click(screen.getByLabelText('Shut down this controller after all other steps complete'))
    expect(screen.getByText('Save Settings')).toBeInTheDocument()
  })
})

/* ══════════════════════════════════════════════════════
 *  CRUD Execution
 * ══════════════════════════════════════════════════════ */

describe('ShutdownConfig — CRUD', () => {
  function mockCrud() {
    globalThis.fetch = vi.fn().mockImplementation((url: string, opts?: RequestInit) => {
      /* Mutations return success */
      if (opts?.method === 'POST' || opts?.method === 'PUT' || opts?.method === 'DELETE') {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve({ ok: true }) })
      }
      /* GET endpoints */
      if (url.includes('/api/shutdown/groups')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(mockGroups) })
      }
      if (url.includes('/api/shutdown/targets')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(mockTargets) })
      }
      if (url.includes('/api/shutdown/settings')) {
        return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(mockSettings) })
      }
      return Promise.resolve({ ok: false, status: 404, json: () => Promise.resolve({}) })
    })
  }

  it('creates a new group', async () => {
    mockCrud()
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getByText('+ Add Group')).toBeInTheDocument()
    })

    await userEvent.click(screen.getByText('+ Add Group'))
    expect(screen.getByText('New Group')).toBeInTheDocument()

    /* Fill in name */
    const nameInput = screen.getAllByRole('textbox')[0]
    await userEvent.type(nameInput, 'Storage')

    await userEvent.click(screen.getByText('Create'))

    /* Verify POST was called */
    await waitFor(() => {
      const calls = (globalThis.fetch as ReturnType<typeof vi.fn>).mock.calls
      const postCall = calls.find((call: unknown[]) => {
        const opts = call[1] as RequestInit | undefined
        return opts?.method === 'POST' && JSON.parse(opts.body as string).name === 'Storage'
      })
      expect(postCall).toBeDefined()
    })
  })

  it('saves settings after changes', async () => {
    mockCrud()
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getByLabelText('Shut down this controller after all other steps complete')).toBeInTheDocument()
    })

    /* Toggle controller to mark dirty */
    await userEvent.click(screen.getByLabelText('Shut down this controller after all other steps complete'))
    expect(screen.getByText('Save Settings')).toBeInTheDocument()

    await userEvent.click(screen.getByText('Save Settings'))

    await waitFor(() => {
      const calls = (globalThis.fetch as ReturnType<typeof vi.fn>).mock.calls
      const settingsPost = calls.find((call: unknown[]) => {
        const [url, opts] = call as [string, RequestInit?]
        return url.includes('/api/shutdown/settings') && opts?.method === 'POST'
      })
      expect(settingsPost).toBeDefined()
    })
  })

  it('adds a target to a group', async () => {
    mockCrud()
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getAllByText('+ Add Target').length).toBe(2)
    })

    await userEvent.click(screen.getAllByText('+ Add Target')[0])

    /* Fill target name */
    const nameInputs = screen.getAllByRole('textbox')
    const targetName = nameInputs.find(i => (i as HTMLInputElement).value === '')
    await userEvent.type(targetName!, 'app-server')

    await userEvent.click(screen.getByRole('button', { name: 'Add Target' }))

    await waitFor(() => {
      const calls = (globalThis.fetch as ReturnType<typeof vi.fn>).mock.calls
      const targetPost = calls.find((call: unknown[]) => {
        const [url, opts] = call as [string, RequestInit?]
        return url.includes('/api/shutdown/targets') && opts?.method === 'POST'
      })
      expect(targetPost).toBeDefined()
    })
  })

  it('shows post-confirm delay on targets that have it', async () => {
    mockAll()
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getByText('+15s')).toBeInTheDocument()
    })
    expect(screen.getByText('+10s')).toBeInTheDocument()
  })
})

/* ══════════════════════════════════════════════════════
 *  Workflow Structure
 * ══════════════════════════════════════════════════════ */

describe('ShutdownConfig — Layout', () => {
  it('renders workflow dividers', async () => {
    mockAll()
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getByText('Workflow')).toBeInTheDocument()
    })
    expect(screen.getByText('Then...')).toBeInTheDocument()
  })

  it('renders page title and description', async () => {
    mockAll()
    renderWithRouter(<ShutdownConfig />)

    await waitFor(() => {
      expect(screen.getByText('Shutdown Configuration')).toBeInTheDocument()
    })
    expect(screen.getByText(/Execution runs top to bottom/)).toBeInTheDocument()
  })
})

/* ══════════════════════════════════════════════════════
 *  Per-target Test buttons (Test SSH / Test Down)
 * ══════════════════════════════════════════════════════ */

/* Stub fetch so GET requests for groups/targets/settings load fixtures
 * and POST requests to test_ssh/test_confirm return the supplied body
 * (recorded so tests can inspect what the UI sent). */
function stubProbe(probeBody: unknown) {
  const calls: { url: string; body: unknown }[] = []
  globalThis.fetch = vi.fn().mockImplementation((url: string, init?: RequestInit) => {
    if (init?.method === 'POST' && url.includes('/api/shutdown/targets/test_')) {
      calls.push({
        url,
        body: init.body ? JSON.parse(init.body as string) : null,
      })
      return Promise.resolve({
        ok: true, status: 200, statusText: 'OK',
        json: () => Promise.resolve(probeBody),
      })
    }
    if (url.includes('/api/shutdown/groups'))
      return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(mockGroups) })
    if (url.includes('/api/shutdown/targets'))
      return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(mockTargets) })
    if (url.includes('/api/shutdown/settings'))
      return Promise.resolve({ ok: true, status: 200, json: () => Promise.resolve(mockSettings) })
    return Promise.resolve({ ok: false, status: 404, json: () => Promise.resolve({}) })
  })
  return calls
}

describe('ShutdownConfig — per-target Test buttons', () => {
  it('renders Test SSH and Test Down buttons for each target', async () => {
    mockAll()
    renderWithRouter(<ShutdownConfig />)
    await waitFor(() => {
      expect(screen.getByText('web-server')).toBeInTheDocument()
    })
    /* Two targets in the fixture -> two of each button. */
    expect(screen.getAllByRole('button', { name: 'Test SSH' })).toHaveLength(2)
    expect(screen.getAllByRole('button', { name: 'Test Down' })).toHaveLength(2)
  })

  it('Test SSH posts the target id and renders ✓ on success', async () => {
    const calls = stubProbe({ ok: true, error: '' })
    renderWithRouter(<ShutdownConfig />)
    await waitFor(() => {
      expect(screen.getByText('web-server')).toBeInTheDocument()
    })

    await userEvent.click(screen.getAllByRole('button', { name: 'Test SSH' })[0])

    await waitFor(() => {
      expect(screen.getByRole('button', { name: 'Test SSH ✓' })).toBeInTheDocument()
    })
    /* First target in mockTargets has id=1; UI must round-trip it verbatim. */
    expect(calls).toHaveLength(1)
    expect(calls[0].url).toContain('/api/shutdown/targets/test_ssh')
    expect(calls[0].body).toEqual({ id: 1 })
  })

  it('Test SSH renders ✗ + tooltip when the probe reports failure', async () => {
    stubProbe({ ok: false, error: 'sshpass not found' })
    renderWithRouter(<ShutdownConfig />)
    await waitFor(() => {
      expect(screen.getByText('web-server')).toBeInTheDocument()
    })

    await userEvent.click(screen.getAllByRole('button', { name: 'Test SSH' })[0])

    const failed = await screen.findByRole('button', { name: 'Test SSH ✗' })
    expect(failed).toHaveAttribute('title', 'sshpass not found')
  })

  it('Test Down posts to test_confirm with the second target id', async () => {
    const calls = stubProbe({ ok: true, error: '' })
    renderWithRouter(<ShutdownConfig />)
    await waitFor(() => {
      expect(screen.getByText('db-server')).toBeInTheDocument()
    })

    /* Click Test Down on the SECOND row to confirm per-row state isolation
     * and that the right id (=2) flows through. */
    await userEvent.click(screen.getAllByRole('button', { name: 'Test Down' })[1])

    await waitFor(() => {
      expect(screen.getByRole('button', { name: 'Test Down ✓' })).toBeInTheDocument()
    })
    expect(calls[0].url).toContain('/api/shutdown/targets/test_confirm')
    expect(calls[0].body).toEqual({ id: 2 })
  })

  it('per-row state is isolated — the other row stays idle', async () => {
    stubProbe({ ok: true, error: '' })
    renderWithRouter(<ShutdownConfig />)
    await waitFor(() => {
      expect(screen.getByText('web-server')).toBeInTheDocument()
    })

    await userEvent.click(screen.getAllByRole('button', { name: 'Test SSH' })[0])

    await waitFor(() => {
      expect(screen.getByRole('button', { name: 'Test SSH ✓' })).toBeInTheDocument()
    })
    /* Second row's Test SSH button should still read "Test SSH" (idle). */
    const sshButtons = screen.getAllByRole('button', { name: /^Test SSH/ })
    expect(sshButtons).toHaveLength(2)
    expect(sshButtons[1]).toHaveTextContent(/^Test SSH$/)
  })
})
