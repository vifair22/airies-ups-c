/* Toast notification system */

import { useState, useCallback } from 'react'
import type { Toast } from '../types/commands'

let toastId = 0

export function useToast() {
  const [toasts, setToasts] = useState<Toast[]>([])
  const push = useCallback((message: string, type: 'success' | 'error') => {
    const id = ++toastId
    setToasts(prev => [...prev, { id, message, type }])
    setTimeout(() => setToasts(prev => prev.filter(t => t.id !== id)), 4000)
  }, [])
  return { toasts, push }
}

export function ToastContainer({ toasts }: { toasts: Toast[] }) {
  if (toasts.length === 0) return null
  return (
    <div className="fixed bottom-4 left-4 right-4 sm:left-auto z-50 space-y-2">
      {toasts.map(t => (
        <div key={t.id} className={`px-4 py-2.5 rounded-lg border text-sm shadow-lg ${
          t.type === 'error'
            ? 'bg-red-600 border-red-700 text-white'
            : 'bg-green-600 border-green-700 text-white'
        }`}>
          {t.message}
        </div>
      ))}
    </div>
  )
}
