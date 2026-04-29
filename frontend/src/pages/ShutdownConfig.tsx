import { useState, useEffect } from 'react'
import { useApi, apiPost, apiPut, apiDelete } from '../hooks/useApi'
import { Field } from '../components/Field'
import { SecretField } from '../components/SecretField'
import type { ShutdownGroup, ShutdownTarget, ShutdownTrigger, ShutdownSettings } from '../types/shutdown'

const EMPTY_TARGET = {
  group_id: 0, name: '', method: 'ssh_key', host: '', username: 'root',
  credential: '', command: 'poweroff', timeout_sec: 180, order_in_group: 0,
  confirm_method: 'ping', confirm_port: 22, confirm_command: '', post_confirm_delay: 15,
}

export default function ShutdownConfig() {
  const { data: groups, refetch: refetchGroups } = useApi<ShutdownGroup[]>('/api/shutdown/groups')
  const { data: targets, refetch: refetchTargets } = useApi<ShutdownTarget[]>('/api/shutdown/targets')
  const { data: settings, refetch: refetchSettings } = useApi<ShutdownSettings>('/api/shutdown/settings')

  const [showAddGroup, setShowAddGroup] = useState(false)
  const [newGroup, setNewGroup] = useState({ name: '', execution_order: 0, parallel: true, max_timeout_sec: 0, post_group_delay: 0 })
  const [editGroupId, setEditGroupId] = useState<number | null>(null)

  const [showAddTarget, setShowAddTarget] = useState<number | null>(null)
  const [newTarget, setNewTarget] = useState({ ...EMPTY_TARGET })
  const [editTargetId, setEditTargetId] = useState<number | null>(null)

  const [trigger, setTrigger] = useState<ShutdownTrigger>({
    mode: 'software', source: 'runtime', runtime_sec: 300, battery_pct: 0,
    on_battery: true, delay_sec: 30, field: '', field_op: 'lt', field_value: 0,
  })
  const [upsAction, setUpsAction] = useState({ mode: 'command', register: '', value: 0, delay: 5 })
  const [ctrlEnabled, setCtrlEnabled] = useState(true)
  const [settingsDirty, setSettingsDirty] = useState(false)

  useEffect(() => {
    if (settings) {
      setTrigger(settings.trigger)
      setUpsAction(settings.ups_action)
      setCtrlEnabled(settings.controller.enabled)
    }
  }, [settings])

  const refetchAll = () => { refetchGroups(); refetchTargets() }

  /* --- Group CRUD --- */
  const addGroup = async () => {
    await apiPost('/api/shutdown/groups', newGroup)
    setShowAddGroup(false)
    setNewGroup({ name: '', execution_order: 0, parallel: true, max_timeout_sec: 0, post_group_delay: 0 })
    refetchGroups()
  }

  const updateGroup = async (g: ShutdownGroup) => {
    await apiPut('/api/shutdown/groups', g)
    setEditGroupId(null)
    refetchGroups()
  }

  const deleteGroup = async (id: number) => {
    if (!confirm('Delete this group and all its targets?')) return
    await apiDelete('/api/shutdown/groups', { id })
    refetchAll()
  }

  /* --- Target CRUD --- */
  const addTarget = async (groupId: number) => {
    await apiPost('/api/shutdown/targets', { ...newTarget, group_id: groupId })
    setShowAddTarget(null)
    setNewTarget({ ...EMPTY_TARGET })
    refetchTargets()
  }

  const updateTarget = async (t: ShutdownTarget) => {
    await apiPut('/api/shutdown/targets', t)
    setEditTargetId(null)
    refetchTargets()
  }

  const deleteTarget = async (id: number) => {
    if (!confirm('Delete this target?')) return
    await apiDelete('/api/shutdown/targets', { id })
    refetchTargets()
  }

  /* --- Settings --- */
  const saveSettings = async () => {
    await apiPost('/api/shutdown/settings', {
      trigger,
      ups_action: upsAction,
      controller: { enabled: ctrlEnabled },
    })
    setSettingsDirty(false)
    refetchSettings()
  }

  const groupTargets = (groupId: number) =>
    (targets || []).filter((t) => t.group_id === groupId)

  return (
    <div>
      <h2 className="text-xl font-semibold mb-1">Shutdown Configuration</h2>
      <p className="text-sm text-muted mb-4">
        Execution runs top to bottom. Groups execute sequentially; targets within a group run in parallel or sequentially.
      </p>

      {/* --- Trigger --- */}
      <div className="rounded-lg bg-panel border border-edge mb-4">
        <div className="px-4 py-2.5 border-b border-edge">
          <h3 className="text-xs font-medium text-muted uppercase tracking-wider">Trigger</h3>
        </div>
        <div className="px-4 py-3 space-y-3">
          <div className="flex items-center gap-3">
            <label className="text-sm text-muted">Mode</label>
            <select value={trigger.mode}
              onChange={(e) => { setTrigger({ ...trigger, mode: e.target.value }); setSettingsDirty(true) }}
              className="bg-field border border-edge-strong rounded px-3 py-1.5 text-sm">
              <option value="software">Automatic - Software</option>
              <option value="ups">Automatic - UPS</option>
              <option value="manual">Manual</option>
            </select>
          </div>

          {trigger.mode === 'software' && (
            <>
              <div className="flex gap-4">
                <label className="flex items-center gap-1.5 text-sm">
                  <input type="radio" name="trigger_source" checked={trigger.source === 'runtime'}
                    onChange={() => { setTrigger({ ...trigger, source: 'runtime' }); setSettingsDirty(true) }} />
                  Runtime / Battery
                </label>
                <label className="flex items-center gap-1.5 text-sm">
                  <input type="radio" name="trigger_source" checked={trigger.source === 'field'}
                    onChange={() => { setTrigger({ ...trigger, source: 'field' }); setSettingsDirty(true) }} />
                  Data Field
                </label>
              </div>

              {trigger.source === 'runtime' && (
                <div className="flex gap-3 items-end flex-wrap">
                  <Field label="Runtime Below (s)" type="number" value={String(trigger.runtime_sec)} width="w-full sm:w-24"
                    onChange={(v) => { setTrigger({ ...trigger, runtime_sec: parseInt(v) || 0 }); setSettingsDirty(true) }} />
                  <Field label="Battery Below (%)" type="number" value={String(trigger.battery_pct)} width="w-full sm:w-24"
                    onChange={(v) => { setTrigger({ ...trigger, battery_pct: parseInt(v) || 0 }); setSettingsDirty(true) }} />
                  <p className="text-xs text-muted pb-1">Set to 0 to disable that condition.</p>
                </div>
              )}

              {trigger.source === 'field' && (
                <div className="space-y-2">
                  <div className="flex gap-3 items-end flex-wrap">
                    <Field label="Field" value={trigger.field} width="w-full sm:w-36"
                      onChange={(v) => { setTrigger({ ...trigger, field: v }); setSettingsDirty(true) }} />
                    <div>
                      <label className="text-xs text-muted">Operator</label>
                      <select value={trigger.field_op}
                        onChange={(e) => { setTrigger({ ...trigger, field_op: e.target.value }); setSettingsDirty(true) }}
                        className="block bg-field border border-edge-strong rounded px-2 py-1 text-sm mt-0.5 w-full sm:w-20">
                        <option value="lt">&lt;</option>
                        <option value="gt">&gt;</option>
                        <option value="eq">=</option>
                      </select>
                    </div>
                    <Field label="Value" type="number" value={String(trigger.field_value)} width="w-full sm:w-20"
                      onChange={(v) => { setTrigger({ ...trigger, field_value: parseInt(v) || 0 }); setSettingsDirty(true) }} />
                  </div>
                  <p className="text-[10px] text-faint">
                    Fields: runtime_sec, charge_pct, battery_voltage, load_pct, output_voltage, input_voltage, output_current, efficiency
                  </p>
                </div>
              )}

              <div className="border-t border-edge pt-3 flex gap-3 items-end flex-wrap">
                <Field label="Debounce (s)" type="number" value={String(trigger.delay_sec)} width="w-full sm:w-20"
                  onChange={(v) => { setTrigger({ ...trigger, delay_sec: parseInt(v) || 0 }); setSettingsDirty(true) }} />
                <label className="flex items-center gap-1 text-sm pb-1">
                  <input type="checkbox" checked={trigger.on_battery}
                    onChange={(e) => { setTrigger({ ...trigger, on_battery: e.target.checked }); setSettingsDirty(true) }} />
                  Require on-battery
                </label>
              </div>
            </>
          )}

          {trigger.mode === 'ups' && (
            <p className="text-xs text-muted">
              Defers to the UPS's internal shutdown-imminent flag. The UPS decides when to trigger based on its LCD-configured runtime threshold.
            </p>
          )}

          {trigger.mode === 'manual' && (
            <p className="text-xs text-muted">
              Shutdown workflow only runs when manually triggered via the CLI or API.
            </p>
          )}
        </div>
      </div>

      {/* --- Divider --- */}
      <div className="flex items-center gap-3 my-6">
        <div className="flex-1 border-t border-edge" />
        <span className="text-xs text-muted uppercase tracking-wider">Workflow</span>
        <div className="flex-1 border-t border-edge" />
      </div>

      {/* --- Groups with nested targets --- */}
      {groups && groups.map((g) => (
        <GroupCard key={g.id} group={g} targets={groupTargets(g.id)}
          editing={editGroupId === g.id}
          onEdit={() => setEditGroupId(g.id)} onCancelEdit={() => setEditGroupId(null)}
          onSave={updateGroup} onDelete={() => deleteGroup(g.id)}
          showAddTarget={showAddTarget === g.id}
          onToggleAddTarget={() => setShowAddTarget(showAddTarget === g.id ? null : g.id)}
          newTarget={newTarget} setNewTarget={setNewTarget}
          onAddTarget={() => addTarget(g.id)}
          editTargetId={editTargetId} setEditTargetId={setEditTargetId}
          onUpdateTarget={updateTarget} onDeleteTarget={deleteTarget}
        />
      ))}

      {/* Add Group button */}
      {!showAddGroup ? (
        <button onClick={() => setShowAddGroup(true)}
          className="text-sm text-accent hover:text-accent-hover mb-6">
          + Add Group
        </button>
      ) : (
        <div className="rounded-lg bg-panel border border-edge p-4 mb-6">
          <h4 className="text-sm font-medium mb-3">New Group</h4>
          <div className="flex gap-3 items-end flex-wrap">
            <Field label="Name" value={newGroup.name}
              onChange={(v) => setNewGroup({ ...newGroup, name: v })} />
            <Field label="Order" type="number" value={String(newGroup.execution_order)} width="w-full sm:w-16"
              onChange={(v) => setNewGroup({ ...newGroup, execution_order: parseInt(v) || 0 })} />
            <Field label="Max Timeout (s)" type="number" value={String(newGroup.max_timeout_sec)} width="w-full sm:w-20"
              onChange={(v) => setNewGroup({ ...newGroup, max_timeout_sec: parseInt(v) || 0 })} />
            <Field label="Post Delay (s)" type="number" value={String(newGroup.post_group_delay)} width="w-full sm:w-20"
              onChange={(v) => setNewGroup({ ...newGroup, post_group_delay: parseInt(v) || 0 })} />
            <label className="flex items-center gap-1 text-sm pb-1">
              <input type="checkbox" checked={newGroup.parallel}
                onChange={(e) => setNewGroup({ ...newGroup, parallel: e.target.checked })} />
              Parallel
            </label>
            <div className="flex gap-2 pb-1">
              <button onClick={addGroup} className="px-3 py-1.5 bg-accent hover:bg-accent-hover text-white rounded text-sm">Create</button>
              <button onClick={() => setShowAddGroup(false)} className="px-3 py-1.5 text-muted hover:text-primary text-sm">Cancel</button>
            </div>
          </div>
        </div>
      )}

      {/* --- Divider --- */}
      <div className="flex items-center gap-3 my-6">
        <div className="flex-1 border-t border-edge" />
        <span className="text-xs text-muted uppercase tracking-wider">Then...</span>
        <div className="flex-1 border-t border-edge" />
      </div>

      {/* --- UPS Action --- */}
      <div className="rounded-lg bg-panel border border-edge mb-4">
        <div className="px-4 py-2.5 border-b border-edge flex items-center justify-between">
          <h3 className="text-xs font-medium text-muted uppercase tracking-wider">UPS Action</h3>
          <span className="text-[10px] text-faint">Fixed — always second-to-last</span>
        </div>
        <div className="px-4 py-3 space-y-3">
          <div className="flex gap-4">
            {(['command', 'register', 'none'] as const).map((m) => (
              <label key={m} className="flex items-center gap-1.5 text-sm">
                <input type="radio" name="ups_mode" checked={upsAction.mode === m}
                  onChange={() => { setUpsAction({ ...upsAction, mode: m }); setSettingsDirty(true) }} />
                {m === 'command' ? 'Send Shutdown Command' : m === 'register' ? 'Write Register' : 'None'}
              </label>
            ))}
          </div>

          {upsAction.mode === 'command' && (
            <p className="text-xs text-muted">Sends the driver's shutdown command to the UPS.</p>
          )}
          {upsAction.mode === 'register' && (
            <div className="flex gap-3 items-end flex-wrap">
              <Field label="Register" value={upsAction.register} width="w-full sm:w-40"
                onChange={(v) => { setUpsAction({ ...upsAction, register: v }); setSettingsDirty(true) }} />
              <Field label="Value" type="number" value={String(upsAction.value)} width="w-full sm:w-20"
                onChange={(v) => { setUpsAction({ ...upsAction, value: parseInt(v) || 0 }); setSettingsDirty(true) }} />
            </div>
          )}
          {upsAction.mode === 'none' && (
            <p className="text-xs text-muted">UPS will not be commanded. Output stays on until battery depletes.</p>
          )}

          {upsAction.mode !== 'none' && (
            <div className="flex items-center gap-2">
              <Field label="Post-Action Delay (s)" type="number" value={String(upsAction.delay)} width="w-full sm:w-20"
                onChange={(v) => { setUpsAction({ ...upsAction, delay: parseInt(v) || 0 }); setSettingsDirty(true) }} />
            </div>
          )}
        </div>
      </div>

      {/* --- Controller --- */}
      <div className="rounded-lg bg-panel border border-edge mb-4">
        <div className="px-4 py-2.5 border-b border-edge flex items-center justify-between">
          <h3 className="text-xs font-medium text-muted uppercase tracking-wider">Controller</h3>
          <span className="text-[10px] text-faint">Fixed — always last</span>
        </div>
        <div className="px-4 py-3">
          <label className="flex items-center gap-2 text-sm">
            <input type="checkbox" checked={ctrlEnabled}
              onChange={(e) => { setCtrlEnabled(e.target.checked); setSettingsDirty(true) }} />
            Shut down this controller after all other steps complete
          </label>
        </div>
      </div>

      {settingsDirty && (
        <button onClick={saveSettings}
          className="px-4 py-1.5 bg-accent hover:bg-accent-hover text-white rounded text-sm">
          Save Settings
        </button>
      )}
    </div>
  )
}

/* --- Group card with nested targets --- */

function GroupCard({ group, targets, editing, onEdit, onCancelEdit, onSave, onDelete,
  showAddTarget, onToggleAddTarget, newTarget, setNewTarget, onAddTarget,
  editTargetId, setEditTargetId, onUpdateTarget, onDeleteTarget,
}: {
  group: ShutdownGroup
  targets: ShutdownTarget[]
  editing: boolean
  onEdit: () => void; onCancelEdit: () => void
  onSave: (g: ShutdownGroup) => void; onDelete: () => void
  showAddTarget: boolean; onToggleAddTarget: () => void
  newTarget: typeof EMPTY_TARGET
  setNewTarget: (t: typeof EMPTY_TARGET) => void
  onAddTarget: () => void
  editTargetId: number | null; setEditTargetId: (id: number | null) => void
  onUpdateTarget: (t: ShutdownTarget) => void; onDeleteTarget: (id: number) => void
}) {
  const [editGroup, setEditGroup] = useState(group)
  useEffect(() => setEditGroup(group), [group])

  return (
    <div className="rounded-lg bg-panel border border-edge mb-4">
      {/* Group header */}
      <div className="px-4 py-2.5 border-b border-edge flex flex-wrap items-center gap-x-3 gap-y-1">
        {editing ? (
          <div className="flex-1 flex gap-2 items-end flex-wrap">
            <Field label="Name" value={editGroup.name} onChange={(v) => setEditGroup({ ...editGroup, name: v })} />
            <Field label="Order" type="number" value={String(editGroup.execution_order)} width="w-full sm:w-16"
              onChange={(v) => setEditGroup({ ...editGroup, execution_order: parseInt(v) || 0 })} />
            <Field label="Max Timeout (s)" type="number" value={String(editGroup.max_timeout_sec)} width="w-full sm:w-20"
              onChange={(v) => setEditGroup({ ...editGroup, max_timeout_sec: parseInt(v) || 0 })} />
            <Field label="Post Delay (s)" type="number" value={String(editGroup.post_group_delay)} width="w-full sm:w-20"
              onChange={(v) => setEditGroup({ ...editGroup, post_group_delay: parseInt(v) || 0 })} />
            <label className="flex items-center gap-1 text-sm pb-1">
              <input type="checkbox" checked={editGroup.parallel}
                onChange={(e) => setEditGroup({ ...editGroup, parallel: e.target.checked })} />
              Parallel
            </label>
            <div className="flex gap-2 pb-1">
              <button onClick={() => onSave(editGroup)} className="text-xs text-green-400 hover:text-green-300">Save</button>
              <button onClick={onCancelEdit} className="text-xs text-muted hover:text-primary">Cancel</button>
            </div>
          </div>
        ) : (
          <>
            <span className="text-sm font-medium flex-1">{group.name}</span>
            <span className={`px-2 py-0.5 rounded text-[10px] ${group.parallel ? 'bg-status-green text-green-400' : 'bg-status-muted text-muted'}`}>
              {group.parallel ? 'parallel' : 'sequential'}
            </span>
            {group.max_timeout_sec > 0 && (
              <span className="text-[10px] text-faint">max {group.max_timeout_sec}s</span>
            )}
            {group.post_group_delay > 0 && (
              <span className="text-[10px] text-faint">+{group.post_group_delay}s delay</span>
            )}
            <button onClick={onEdit} className="text-xs text-muted hover:text-primary">Edit</button>
            <button onClick={onDelete} className="text-xs text-red-400 hover:text-red-300">Delete</button>
          </>
        )}
      </div>

      {/* Targets */}
      <div className="divide-y divide-edge">
        {targets.map((t) => (
          editTargetId === t.id ? (
            <TargetEditRow key={t.id} target={t} onSave={onUpdateTarget}
              onCancel={() => setEditTargetId(null)} />
          ) : (
            <TargetRow key={t.id} target={t} onEdit={() => setEditTargetId(t.id)}
              onDelete={() => onDeleteTarget(t.id)} />
          )
        ))}
        {targets.length === 0 && !showAddTarget && (
          <p className="px-4 py-3 text-xs text-muted">No targets in this group.</p>
        )}
      </div>

      {/* Add target */}
      <div className="px-4 py-2 border-t border-edge">
        {showAddTarget ? (
          <TargetForm target={newTarget} setTarget={setNewTarget as (t: TargetLike) => void}
            onSubmit={onAddTarget} onCancel={onToggleAddTarget} submitLabel="Add Target"
            isEdit={false} />
        ) : (
          <button onClick={onToggleAddTarget} className="text-xs text-accent hover:text-accent-hover">
            + Add Target
          </button>
        )}
      </div>
    </div>
  )
}

/* --- Target display row --- */

function TargetRow({ target, onEdit, onDelete }: {
  target: ShutdownTarget; onEdit: () => void; onDelete: () => void
}) {
  return (
    <div className="px-4 py-2.5 flex flex-wrap items-center gap-x-3 gap-y-1 text-sm">
      <span className="font-medium w-full sm:w-28 sm:shrink-0">{target.name}</span>
      <span className="font-mono text-xs text-muted w-full sm:w-28 sm:shrink-0 truncate">{target.host}</span>
      <span className="px-1.5 py-0.5 rounded text-[10px] bg-status-muted text-muted shrink-0">{target.method}</span>
      <span className="font-mono text-xs text-muted flex-1 min-w-0 truncate">{target.command}</span>
      <span className="text-[10px] text-faint shrink-0">{target.confirm_method} / {target.timeout_sec}s</span>
      {target.post_confirm_delay > 0 && (
        <span className="text-[10px] text-faint shrink-0">+{target.post_confirm_delay}s</span>
      )}
      <button onClick={onEdit} className="text-xs text-muted hover:text-primary shrink-0">Edit</button>
      <button onClick={onDelete} className="text-xs text-red-400 hover:text-red-300 shrink-0">Delete</button>
    </div>
  )
}

/* --- Target edit row --- */

function TargetEditRow({ target, onSave, onCancel }: {
  target: ShutdownTarget; onSave: (t: ShutdownTarget) => void; onCancel: () => void
}) {
  const [t, setT] = useState({ ...target, credential: '' })
  return (
    <div className="px-4 py-3">
      <TargetForm target={t} setTarget={setT as (t: TargetLike) => void}
        onSubmit={() => onSave(t)} onCancel={onCancel} submitLabel="Save"
        isEdit={true} />
    </div>
  )
}

/* --- Shared target form --- */

type TargetLike = ShutdownTarget | typeof EMPTY_TARGET

function TargetForm({ target, setTarget, onSubmit, onCancel, submitLabel, isEdit }: {
  target: TargetLike
  setTarget: (t: TargetLike) => void
  onSubmit: () => void; onCancel: () => void; submitLabel: string
  isEdit: boolean
}) {
  const t = target
  const set = (k: string, v: string | number) => setTarget({ ...target, [k]: v } as TargetLike)

  const credPlaceholder = isEdit ? 'leave blank to keep existing' : ''

  return (
    <div className="space-y-3">
      <div className="grid grid-cols-1 sm:grid-cols-3 gap-3">
        <Field label="Name" value={t.name} onChange={(v) => set('name', v)} />
        <Field label="Host" value={t.host} onChange={(v) => set('host', v)} />
        <Field label="Username" value={t.username} onChange={(v) => set('username', v)} />
      </div>
      <div className="grid grid-cols-1 sm:grid-cols-3 gap-3">
        <div>
          <label className="text-xs text-muted">Action Method</label>
          <select value={t.method} onChange={(e) => set('method', e.target.value)}
            className="block bg-field border border-edge-strong rounded px-2 py-1 text-sm mt-0.5 w-full">
            <option value="ssh_password">SSH Password</option>
            <option value="ssh_key">SSH Key</option>
            <option value="command">Command</option>
          </select>
        </div>
        <Field label={t.method === 'command' ? 'Command' : 'Shutdown Command'} value={t.command} onChange={(v) => set('command', v)} />
        {t.method === 'ssh_password' && (
          <SecretField label="Password" value={t.credential || ''}
            onChange={(v) => set('credential', v)} placeholder={credPlaceholder} />
        )}
        {t.method === 'ssh_key' && (
          <div className="sm:col-span-1">
            <SecretField label="Private Key (PEM)" multiline rows={5}
              value={t.credential || ''} onChange={(v) => set('credential', v)}
              placeholder={credPlaceholder || 'paste contents of private key file'} />
          </div>
        )}
      </div>
      <div className="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-4 gap-3">
        <div>
          <label className="text-xs text-muted">Confirmation</label>
          <select value={t.confirm_method} onChange={(e) => set('confirm_method', e.target.value)}
            className="block bg-field border border-edge-strong rounded px-2 py-1 text-sm mt-0.5 w-full">
            <option value="ping">Ping</option>
            <option value="tcp_port">TCP Port</option>
            <option value="command">Command</option>
            <option value="none">None</option>
          </select>
        </div>
        {t.confirm_method === 'tcp_port' && (
          <Field label="Port" type="number" value={String(t.confirm_port || 22)} width="w-full"
            onChange={(v) => set('confirm_port', parseInt(v) || 0)} />
        )}
        {t.confirm_method === 'command' && (
          <Field label="Confirm Command" value={t.confirm_command || ''} width="w-full"
            onChange={(v) => set('confirm_command', v)} />
        )}
        <Field label="Timeout (s)" type="number" value={String(t.timeout_sec)} width="w-full"
          onChange={(v) => set('timeout_sec', parseInt(v) || 180)} />
        <Field label="Post-Confirm Delay (s)" type="number" value={String(t.post_confirm_delay)} width="w-full"
          onChange={(v) => set('post_confirm_delay', parseInt(v) || 0)} />
      </div>
      <div className="flex gap-2">
        <button onClick={onSubmit} className="px-3 py-1.5 bg-accent hover:bg-accent-hover text-white rounded text-sm">{submitLabel}</button>
        <button onClick={onCancel} className="px-3 py-1.5 text-muted hover:text-primary text-sm">Cancel</button>
      </div>
    </div>
  )
}
