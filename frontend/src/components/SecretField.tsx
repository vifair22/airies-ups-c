import { useState } from 'react'

/* Credential entry — masked by default with show/hide toggle.
 *
 * multiline=true renders a textarea masked via -webkit-text-security
 * (Chromium/Safari/Edge support; Firefox shows plaintext while typing,
 * which is acceptable since the operator is pasting their own key). */

export function SecretField({ label, value, onChange, multiline, placeholder, rows = 6 }: {
  label: string
  value: string
  onChange: (v: string) => void
  multiline?: boolean
  placeholder?: string
  rows?: number
}) {
  const [revealed, setRevealed] = useState(false)
  const baseCls = 'block bg-field border border-edge-strong rounded px-2 py-1 text-sm mt-0.5 w-full'

  return (
    <div>
      <div className="flex items-center justify-between">
        <label className="text-xs text-muted">{label}</label>
        <button type="button" onClick={() => setRevealed(!revealed)}
          className="text-[10px] text-muted hover:text-primary">
          {revealed ? 'hide' : 'show'}
        </button>
      </div>
      {multiline ? (
        <textarea value={value} onChange={(e) => onChange(e.target.value)}
          placeholder={placeholder} rows={rows} spellCheck={false}
          autoComplete="off" data-masked={!revealed}
          className={`${baseCls} font-mono`}
          style={revealed ? undefined : { WebkitTextSecurity: 'disc' } as React.CSSProperties} />
      ) : (
        <input type={revealed ? 'text' : 'password'} value={value}
          onChange={(e) => onChange(e.target.value)} placeholder={placeholder}
          autoComplete="off" data-masked={!revealed} className={baseCls} />
      )}
    </div>
  )
}
