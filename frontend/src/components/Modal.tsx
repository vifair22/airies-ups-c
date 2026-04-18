/* Reusable modal overlay */

export function Modal({ children, onClose }: {
  children: React.ReactNode
  onClose: () => void
}) {
  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/60" onClick={onClose}>
      <div className="bg-panel border border-edge-strong rounded-lg p-6 max-w-md mx-4 shadow-xl"
        onClick={e => e.stopPropagation()}>
        {children}
      </div>
    </div>
  )
}

export function WideModal({ children, onClose }: {
  children: React.ReactNode
  onClose: () => void
}) {
  return (
    <div className="fixed inset-0 bg-black/50 flex items-center justify-center z-50 overflow-auto" onClick={onClose}>
      <div className="bg-panel border border-edge rounded-lg p-6 w-full max-w-2xl my-8 max-h-[90vh] overflow-auto"
        onClick={e => e.stopPropagation()}>
        {children}
      </div>
    </div>
  )
}

export function ConfirmModal({ title, body, confirmLabel, confirmVariant = 'default', onConfirm, onCancel, loading }: {
  title: string; body: string; confirmLabel: string
  confirmVariant?: 'default' | 'warn' | 'danger'
  onConfirm: () => void; onCancel: () => void; loading?: boolean
}) {
  const confirmBg = confirmVariant === 'danger'
    ? 'bg-red-700 hover:bg-red-600 text-white border-red-800'
    : confirmVariant === 'warn'
      ? 'bg-yellow-600 hover:bg-yellow-700 text-white border-yellow-700'
      : 'bg-field hover:bg-field-hover border-edge-strong'
  const borderColor = confirmVariant === 'danger' ? 'border-red-800' : 'border-edge-strong'

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/60" onClick={onCancel}>
      <div className={`bg-panel border ${borderColor} rounded-lg p-6 max-w-md mx-4 shadow-xl`} onClick={e => e.stopPropagation()}>
        <h3 className={`text-lg font-semibold mb-2 ${confirmVariant === 'danger' ? 'text-red-400' : 'text-primary'}`}>{title}</h3>
        <p className="text-sm text-muted mb-4">{body}</p>
        <div className="flex justify-end gap-3">
          <button onClick={onCancel}
            className="px-4 py-2 rounded text-sm border border-edge-strong bg-field hover:bg-field-hover transition-colors">Cancel</button>
          <button onClick={onConfirm} disabled={loading}
            className={`px-4 py-2 rounded text-sm border transition-colors disabled:opacity-50 ${confirmBg}`}>
            {loading ? '...' : confirmLabel}
          </button>
        </div>
      </div>
    </div>
  )
}
