import { screen, waitFor } from '@testing-library/react'
import userEvent from '@testing-library/user-event'
import { describe, it, expect, vi, beforeEach } from 'vitest'
import { renderWithRouter, mockApiResponses } from '../test/helpers'
import Commands from './Commands'

beforeEach(() => {
  vi.restoreAllMocks()
})

const mockCommands = [
  {
    name: 'self_test',
    display_name: 'Self Test',
    description: 'Run a quick self-test',
    group: 'diagnostics',
    confirm_title: 'Run Self Test?',
    confirm_body: 'This will run a brief battery self-test.',
    type: 'simple',
    variant: 'default',
  },
  {
    name: 'shutdown',
    display_name: 'Shutdown',
    description: 'Shut down the UPS output',
    group: 'power',
    confirm_title: 'Shut Down UPS?',
    confirm_body: 'This will shut down the UPS output.',
    type: 'simple',
    variant: 'danger',
  },
  {
    name: 'he_mode',
    display_name: 'High Efficiency',
    description: 'Toggle high efficiency mode',
    group: 'power',
    confirm_title: 'Toggle HE Mode?',
    confirm_body: 'This changes the UPS operating mode.',
    type: 'toggle',
    variant: 'warn',
    status_bit: 1 << 13,
  },
]

describe('Commands', () => {
  it('shows disconnected message when UPS is not connected', async () => {
    mockApiResponses({
      '/api/status': { connected: false },
      '/api/commands': [],
    })
    renderWithRouter(<Commands />)

    await waitFor(() => {
      expect(screen.getByText('UPS not connected. Commands unavailable.')).toBeInTheDocument()
    })
  })

  it('renders command groups and buttons', async () => {
    mockApiResponses({
      '/api/status': { connected: true, status: { raw: 2 } },
      '/api/commands': mockCommands,
    })
    renderWithRouter(<Commands />)

    await waitFor(() => {
      expect(screen.getByText('Diagnostics')).toBeInTheDocument()
    })
    expect(screen.getByText('Power Control')).toBeInTheDocument()
    expect(screen.getByText('Run a quick self-test')).toBeInTheDocument()
  })

  it('shows confirm modal when command button is clicked', async () => {
    mockApiResponses({
      '/api/status': { connected: true, status: { raw: 2 } },
      '/api/commands': mockCommands,
    })
    renderWithRouter(<Commands />)

    await waitFor(() => {
      expect(screen.getByText('Run a quick self-test')).toBeInTheDocument()
    })

    /* Click the Self Test button (the <button>, not the label <span>) */
    const buttons = screen.getAllByRole('button', { name: 'Self Test' })
    await userEvent.click(buttons[0])
    expect(screen.getByText('Run Self Test?')).toBeInTheDocument()
    expect(screen.getByText('This will run a brief battery self-test.')).toBeInTheDocument()
  })

  it('toggle command shows active/off state', async () => {
    /* HE mode is active (bit 13 set) */
    mockApiResponses({
      '/api/status': { connected: true, status: { raw: 1 << 13 } },
      '/api/commands': mockCommands,
    })
    renderWithRouter(<Commands />)

    await waitFor(() => {
      expect(screen.getByText('ACTIVE')).toBeInTheDocument()
    })
    expect(screen.getByText('Disable High Efficiency')).toBeInTheDocument()
  })

  it('toggle command shows OFF when bit is not set', async () => {
    mockApiResponses({
      '/api/status': { connected: true, status: { raw: 2 } },
      '/api/commands': mockCommands,
    })
    renderWithRouter(<Commands />)

    await waitFor(() => {
      expect(screen.getByText('OFF')).toBeInTheDocument()
    })
    expect(screen.getByText('Enable High Efficiency')).toBeInTheDocument()
  })

  it('executes simple command and shows toast', async () => {
    globalThis.fetch = vi.fn().mockImplementation((url: string) => {
      if (url.includes('/api/cmd')) {
        return Promise.resolve({
          ok: true, status: 200,
          json: () => Promise.resolve({ result: 'Self test started' }),
        })
      }
      if (url.includes('/api/status')) {
        return Promise.resolve({
          ok: true, status: 200,
          json: () => Promise.resolve({ connected: true, status: { raw: 2 } }),
        })
      }
      return Promise.resolve({
        ok: true, status: 200,
        json: () => Promise.resolve(mockCommands),
      })
    })

    renderWithRouter(<Commands />)

    await waitFor(() => {
      expect(screen.getByText('Run a quick self-test')).toBeInTheDocument()
    })

    /* Open confirm modal */
    const buttons = screen.getAllByRole('button', { name: 'Self Test' })
    await userEvent.click(buttons[0])
    expect(screen.getByText('Run Self Test?')).toBeInTheDocument()

    /* Confirm */
    await userEvent.click(screen.getAllByRole('button', { name: 'Self Test' })[1])

    await waitFor(() => {
      expect(screen.getByText('Self test started')).toBeInTheDocument()
    })
  })

  it('shows error toast on command failure', async () => {
    globalThis.fetch = vi.fn().mockImplementation((url: string) => {
      if (url.includes('/api/cmd')) {
        return Promise.resolve({
          ok: true, status: 200,
          json: () => Promise.resolve({ error: 'UPS rejected command' }),
        })
      }
      if (url.includes('/api/status')) {
        return Promise.resolve({
          ok: true, status: 200,
          json: () => Promise.resolve({ connected: true, status: { raw: 2 } }),
        })
      }
      return Promise.resolve({
        ok: true, status: 200,
        json: () => Promise.resolve(mockCommands),
      })
    })

    renderWithRouter(<Commands />)

    await waitFor(() => {
      expect(screen.getByText('Run a quick self-test')).toBeInTheDocument()
    })

    const buttons = screen.getAllByRole('button', { name: 'Self Test' })
    await userEvent.click(buttons[0])
    await userEvent.click(screen.getAllByRole('button', { name: 'Self Test' })[1])

    await waitFor(() => {
      expect(screen.getByText('UPS rejected command')).toBeInTheDocument()
    })
  })

  it('renders shutdown workflow with dry run and shutdown buttons', async () => {
    mockApiResponses({
      '/api/status': { connected: true, status: { raw: 2 } },
      '/api/commands': mockCommands,
    })
    renderWithRouter(<Commands />)

    await waitFor(() => {
      expect(screen.getByText('Shutdown Workflow')).toBeInTheDocument()
    })
    expect(screen.getByText('Dry Run')).toBeInTheDocument()
    /* There are multiple "Shutdown" texts — the command button and the workflow button */
    expect(screen.getAllByRole('button', { name: 'Shutdown' }).length).toBeGreaterThanOrEqual(1)
  })

  it('toggle command opens confirm modal for enable', async () => {
    mockApiResponses({
      '/api/status': { connected: true, status: { raw: 2 } },
      '/api/commands': mockCommands,
    })
    renderWithRouter(<Commands />)

    await waitFor(() => {
      expect(screen.getByText('Enable High Efficiency')).toBeInTheDocument()
    })

    await userEvent.click(screen.getByText('Enable High Efficiency'))
    expect(screen.getByText('Enable High Efficiency?')).toBeInTheDocument()
  })

  /* ── Shutdown workflow: start + polled status ── */

  /* The workflow is async on the daemon side: POST /api/cmd returns
   * 202 + { result: 'started', workflow_id, dry_run }, and the UI polls
   * GET /api/shutdown/workflow/status for live progress and the final
   * step list. These stubs let each test specify the start response and
   * (optionally) one or more status responses returned in order. */
  type StartResponse = {
    status?: number
    body: object
  }
  type StatusResponse = object  /* WorkflowStatus shape */

  function stubShutdownWorkflow(start: StartResponse, statuses: StatusResponse[]) {
    let nStatusCall = 0
    globalThis.fetch = vi.fn().mockImplementation((url: string) => {
      if (url.includes('/api/shutdown/workflow/status')) {
        const idx = Math.min(nStatusCall, statuses.length - 1)
        nStatusCall++
        return Promise.resolve({
          ok: true, status: 200,
          json: () => Promise.resolve(statuses[idx]),
        })
      }
      if (url.includes('/api/cmd')) {
        return Promise.resolve({
          ok: (start.status ?? 200) < 400,
          status: start.status ?? 202,
          json: () => Promise.resolve(start.body),
        })
      }
      if (url.includes('/api/status')) {
        return Promise.resolve({
          ok: true, status: 200,
          json: () => Promise.resolve({ connected: true, status: { raw: 2 } }),
        })
      }
      return Promise.resolve({
        ok: true, status: 200,
        json: () => Promise.resolve(mockCommands),
      })
    })
  }

  /* Click Dry Run, then confirm in the modal. */
  async function runDryRun() {
    await waitFor(() => {
      expect(screen.getByText('Shutdown Workflow')).toBeInTheDocument()
    })
    await userEvent.click(screen.getByRole('button', { name: 'Dry Run' }))
    expect(screen.getByText('Run Shutdown Dry Run?')).toBeInTheDocument()
    await userEvent.click(screen.getByRole('button', { name: 'Run Dry Run' }))
  }

  /* Polling cadence is 1.5s — give waitFor enough headroom that one
   * poll lands deterministically without slowing CI. */
  const POLL_TIMEOUT = { timeout: 4000 }

  it('opens result modal with per-step rows when status converges to completed', async () => {
    stubShutdownWorkflow(
      { status: 202, body: { result: 'started', dry_run: true, workflow_id: 1 } },
      [{
        state: 'completed', workflow_id: 1, dry_run: true,
        started_at: 1, finished_at: 2, current_phase: '', current_target: '',
        n_steps: 3, n_failed: 0, all_ok: true,
        steps: [
          { phase: 'phase1', target: 'PFsense/IRTR1', ok: 0, error: '' },
          { phase: 'phase2', target: 'ups',           ok: 0, error: '' },
          { phase: 'phase3', target: 'controller',    ok: 2, error: '' },
        ],
      }],
    )
    renderWithRouter(<Commands />)
    await runDryRun()

    await waitFor(() => {
      expect(screen.getByText('Dry run complete — no problems detected')).toBeInTheDocument()
    }, POLL_TIMEOUT)
    expect(screen.getByText('PFsense/IRTR1')).toBeInTheDocument()
    /* Status pills: 2 "ok", 1 "skipped", 0 "failed". */
    expect(screen.getAllByText('ok')).toHaveLength(2)
    expect(screen.getByText('skipped')).toBeInTheDocument()
  })

  it('headlines failures and renders the failing step with its error', async () => {
    stubShutdownWorkflow(
      { status: 202, body: { result: 'started', dry_run: true, workflow_id: 7 } },
      [{
        state: 'completed', workflow_id: 7, dry_run: true,
        started_at: 1, finished_at: 2, current_phase: '', current_target: '',
        n_steps: 2, n_failed: 1, all_ok: false,
        steps: [
          { phase: 'phase1', target: 'PFsense/IRTR1', ok: 1,
            error: 'ssh_password probe to admin@172.20.0.1 failed' },
          { phase: 'phase3', target: 'controller',    ok: 0, error: '' },
        ],
      }],
    )
    renderWithRouter(<Commands />)
    await runDryRun()

    await waitFor(() => {
      expect(screen.getByText('Dry run found 1 problem')).toBeInTheDocument()
    }, POLL_TIMEOUT)
    expect(screen.getByText('failed')).toBeInTheDocument()
    expect(screen.getByText(/ssh_password probe to admin@172.20.0.1 failed/)).toBeInTheDocument()
  })

  it('shows running state with current target before completion', async () => {
    stubShutdownWorkflow(
      { status: 202, body: { result: 'started', dry_run: true, workflow_id: 11 } },
      [
        {
          state: 'running', workflow_id: 11, dry_run: true,
          started_at: 1, finished_at: 0,
          current_phase: 'phase1', current_target: 'Group 1/proxmox-01',
          n_steps: 1, n_failed: 0, all_ok: false,
          steps: [{ phase: 'phase1', target: 'Group 1/proxmox-01', ok: 0, error: '' }],
        },
      ],
    )
    renderWithRouter(<Commands />)
    await runDryRun()

    await waitFor(() => {
      expect(screen.getByText('Dry run in progress…')).toBeInTheDocument()
    }, POLL_TIMEOUT)
    expect(screen.getByText(/Hosts → Group 1\/proxmox-01/)).toBeInTheDocument()
  })

  it('shows error toast when start fails without a workflow_id', async () => {
    stubShutdownWorkflow(
      { status: 503, body: { error: 'shutdown manager unavailable' } },
      [],
    )
    renderWithRouter(<Commands />)
    await runDryRun()

    await waitFor(() => {
      expect(screen.getByText('shutdown manager unavailable')).toBeInTheDocument()
    })
    expect(screen.queryByText(/Dry run complete/)).not.toBeInTheDocument()
    expect(screen.queryByText(/Dry run in progress/)).not.toBeInTheDocument()
  })

  it('attaches to an in-flight workflow when start returns 409', async () => {
    stubShutdownWorkflow(
      { status: 409, body: { error: 'workflow already running', workflow_id: 4 } },
      [{
        state: 'running', workflow_id: 4, dry_run: false,
        started_at: 1, finished_at: 0,
        current_phase: 'phase2', current_target: 'ups',
        n_steps: 1, n_failed: 0, all_ok: false,
        steps: [{ phase: 'phase1', target: 'a/b', ok: 0, error: '' }],
      }],
    )
    renderWithRouter(<Commands />)
    await runDryRun()

    await waitFor(() => {
      expect(screen.getByText('Shutdown workflow in progress…')).toBeInTheDocument()
    }, POLL_TIMEOUT)
    expect(screen.getByText(/UPS → ups/)).toBeInTheDocument()
  })

  it('closes the result modal when Close is clicked', async () => {
    stubShutdownWorkflow(
      { status: 202, body: { result: 'started', dry_run: true, workflow_id: 2 } },
      [{
        state: 'completed', workflow_id: 2, dry_run: true,
        started_at: 1, finished_at: 2, current_phase: '', current_target: '',
        n_steps: 1, n_failed: 0, all_ok: true,
        steps: [{ phase: 'phase2', target: 'ups', ok: 0, error: '' }],
      }],
    )
    renderWithRouter(<Commands />)
    await runDryRun()

    await waitFor(() => {
      expect(screen.getByText('Dry run complete — no problems detected')).toBeInTheDocument()
    }, POLL_TIMEOUT)
    await userEvent.click(screen.getByRole('button', { name: 'Close' }))
    await waitFor(() => {
      expect(screen.queryByText('Dry run complete — no problems detected')).not.toBeInTheDocument()
    })
  })

  it('reports plural failure copy for multi-failure runs', async () => {
    stubShutdownWorkflow(
      { status: 202, body: { result: 'started', dry_run: false, workflow_id: 3 } },
      [{
        state: 'completed', workflow_id: 3, dry_run: false,
        started_at: 1, finished_at: 2, current_phase: '', current_target: '',
        n_steps: 3, n_failed: 2, all_ok: false,
        steps: [
          { phase: 'phase1', target: 'g/a',        ok: 1, error: 'boom' },
          { phase: 'phase1', target: 'g/b',        ok: 1, error: 'boom' },
          { phase: 'phase3', target: 'controller', ok: 0, error: '' },
        ],
      }],
    )
    renderWithRouter(<Commands />)
    await waitFor(() => {
      expect(screen.getByText('Shutdown Workflow')).toBeInTheDocument()
    })
    /* "Shutdown" matches both the UPS power-control command button and
     * the workflow's red button — the workflow button is the last one. */
    const shutdownButtons = screen.getAllByRole('button', { name: 'Shutdown' })
    await userEvent.click(shutdownButtons[shutdownButtons.length - 1])
    await userEvent.click(screen.getByRole('button', { name: 'Shutdown Now' }))

    await waitFor(() => {
      expect(screen.getByText('Shutdown completed with 2 failures')).toBeInTheDocument()
    }, POLL_TIMEOUT)
  })
})
