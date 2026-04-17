import { useState } from 'react'
import { useApi, apiPost } from '../hooks/useApi'

interface ShutdownGroup {
  id: number
  name: string
  execution_order: number
  parallel: boolean
}

interface ShutdownTarget {
  id: number
  name: string
  method: string
  host: string
  username: string
  command: string
  timeout_sec: number
  order_in_group: number
  group: string
}

export default function ShutdownConfig() {
  const { data: groups, refetch: refetchGroups } = useApi<ShutdownGroup[]>('/api/shutdown/groups')
  const { data: targets, refetch: refetchTargets } = useApi<ShutdownTarget[]>('/api/shutdown/targets')

  const [newGroup, setNewGroup] = useState({ name: '', execution_order: 0, parallel: true })
  const [newTarget, setNewTarget] = useState({
    group_id: 0, name: '', method: 'ssh_password', host: '', username: 'root',
    credential: '', command: 'powerdown', timeout_sec: 180,
  })
  const [showAddGroup, setShowAddGroup] = useState(false)
  const [showAddTarget, setShowAddTarget] = useState(false)

  const addGroup = async () => {
    await apiPost('/api/shutdown/groups', newGroup)
    setShowAddGroup(false)
    setNewGroup({ name: '', execution_order: 0, parallel: true })
    refetchGroups()
  }

  const addTarget = async () => {
    await apiPost('/api/shutdown/targets', newTarget)
    setShowAddTarget(false)
    refetchTargets()
  }

  return (
    <div>
      <h2 className="text-xl font-semibold mb-4">Shutdown Configuration</h2>

      <div className="mb-6">
        <div className="flex items-center justify-between mb-2">
          <h3 className="text-sm font-medium text-muted uppercase tracking-wider">Groups</h3>
          <button onClick={() => setShowAddGroup(!showAddGroup)}
            className="text-xs text-blue-400 hover:text-blue-300">
            {showAddGroup ? 'Cancel' : '+ Add Group'}
          </button>
        </div>

        {showAddGroup && (
          <div className="rounded-lg bg-panel border border-edge p-4 mb-4 flex gap-3 items-end">
            <div>
              <label className="text-xs text-muted">Name</label>
              <input value={newGroup.name} onChange={(e) => setNewGroup({ ...newGroup, name: e.target.value })}
                className="block bg-field border border-edge-strong rounded px-2 py-1 text-sm w-40" />
            </div>
            <div>
              <label className="text-xs text-muted">Order</label>
              <input type="number" value={newGroup.execution_order}
                onChange={(e) => setNewGroup({ ...newGroup, execution_order: parseInt(e.target.value) })}
                className="block bg-field border border-edge-strong rounded px-2 py-1 text-sm w-16" />
            </div>
            <label className="flex items-center gap-1 text-sm">
              <input type="checkbox" checked={newGroup.parallel}
                onChange={(e) => setNewGroup({ ...newGroup, parallel: e.target.checked })} />
              Parallel
            </label>
            <button onClick={addGroup} className="px-3 py-1 bg-accent hover:bg-accent-hover text-white rounded text-sm">
              Create
            </button>
          </div>
        )}

        {groups && groups.length > 0 ? (
          <div className="rounded-lg border border-edge overflow-hidden">
            <table className="w-full text-sm">
              <thead className="bg-panel text-muted">
                <tr>
                  <th className="text-left px-4 py-2">Name</th>
                  <th className="text-left px-4 py-2">Order</th>
                  <th className="text-left px-4 py-2">Mode</th>
                </tr>
              </thead>
              <tbody>
                {groups.map((g) => (
                  <tr key={g.id} className="border-t border-edge">
                    <td className="px-4 py-2">{g.name}</td>
                    <td className="px-4 py-2 text-muted">{g.execution_order}</td>
                    <td className="px-4 py-2">
                      <span className={`px-2 py-0.5 rounded text-xs ${g.parallel ? 'bg-green-900 text-green-400' : 'bg-gray-700 text-gray-300'}`}>
                        {g.parallel ? 'parallel' : 'sequential'}
                      </span>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        ) : (
          <p className="text-muted text-sm">No shutdown groups configured.</p>
        )}
      </div>

      <div>
        <div className="flex items-center justify-between mb-2">
          <h3 className="text-sm font-medium text-muted uppercase tracking-wider">Targets</h3>
          <button onClick={() => setShowAddTarget(!showAddTarget)}
            className="text-xs text-blue-400 hover:text-blue-300">
            {showAddTarget ? 'Cancel' : '+ Add Target'}
          </button>
        </div>

        {showAddTarget && groups && (
          <div className="rounded-lg bg-panel border border-edge p-4 mb-4 grid grid-cols-2 gap-3">
            <div>
              <label className="text-xs text-muted">Group</label>
              <select value={newTarget.group_id}
                onChange={(e) => setNewTarget({ ...newTarget, group_id: parseInt(e.target.value) })}
                className="block bg-field border border-edge-strong rounded px-2 py-1 text-sm w-full">
                <option value={0}>Select...</option>
                {groups.map((g) => <option key={g.id} value={g.id}>{g.name}</option>)}
              </select>
            </div>
            <div>
              <label className="text-xs text-muted">Name</label>
              <input value={newTarget.name} onChange={(e) => setNewTarget({ ...newTarget, name: e.target.value })}
                className="block bg-field border border-edge-strong rounded px-2 py-1 text-sm w-full" />
            </div>
            <div>
              <label className="text-xs text-muted">Host</label>
              <input value={newTarget.host} onChange={(e) => setNewTarget({ ...newTarget, host: e.target.value })}
                className="block bg-field border border-edge-strong rounded px-2 py-1 text-sm w-full" />
            </div>
            <div>
              <label className="text-xs text-muted">Username</label>
              <input value={newTarget.username} onChange={(e) => setNewTarget({ ...newTarget, username: e.target.value })}
                className="block bg-field border border-edge-strong rounded px-2 py-1 text-sm w-full" />
            </div>
            <div>
              <label className="text-xs text-muted">Method</label>
              <select value={newTarget.method}
                onChange={(e) => setNewTarget({ ...newTarget, method: e.target.value })}
                className="block bg-field border border-edge-strong rounded px-2 py-1 text-sm w-full">
                <option value="ssh_password">SSH Password</option>
                <option value="ssh_key">SSH Key</option>
                <option value="command">Command</option>
              </select>
            </div>
            <div>
              <label className="text-xs text-muted">Command</label>
              <input value={newTarget.command} onChange={(e) => setNewTarget({ ...newTarget, command: e.target.value })}
                className="block bg-field border border-edge-strong rounded px-2 py-1 text-sm w-full" />
            </div>
            <div className="col-span-2">
              <button onClick={addTarget} className="px-3 py-1.5 bg-accent hover:bg-accent-hover text-white rounded text-sm">
                Create Target
              </button>
            </div>
          </div>
        )}

        {targets && targets.length > 0 ? (
          <div className="rounded-lg border border-edge overflow-hidden">
            <table className="w-full text-sm">
              <thead className="bg-panel text-muted">
                <tr>
                  <th className="text-left px-4 py-2">Name</th>
                  <th className="text-left px-4 py-2">Group</th>
                  <th className="text-left px-4 py-2">Host</th>
                  <th className="text-left px-4 py-2">Method</th>
                  <th className="text-left px-4 py-2">Command</th>
                  <th className="text-left px-4 py-2">Timeout</th>
                </tr>
              </thead>
              <tbody>
                {targets.map((t) => (
                  <tr key={t.id} className="border-t border-edge">
                    <td className="px-4 py-2">{t.name}</td>
                    <td className="px-4 py-2 text-muted">{t.group}</td>
                    <td className="px-4 py-2 font-mono text-xs">{t.host}</td>
                    <td className="px-4 py-2">
                      <span className="px-2 py-0.5 rounded text-xs bg-gray-700">{t.method}</span>
                    </td>
                    <td className="px-4 py-2 font-mono text-xs text-muted">{t.command}</td>
                    <td className="px-4 py-2 text-muted">{t.timeout_sec}s</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        ) : (
          <p className="text-muted text-sm">No shutdown targets configured.</p>
        )}
      </div>
    </div>
  )
}
