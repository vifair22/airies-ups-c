/* Reusable form field — label + input */

export function Field({ label, value, onChange, type = 'text', width }: {
  label: string; value: string; onChange: (v: string) => void
  type?: string; width?: string
}) {
  return (
    <div>
      <label className="text-xs text-muted">{label}</label>
      <input type={type} value={value} onChange={(e) => onChange(e.target.value)}
        className={`block bg-field border border-edge-strong rounded px-2 py-1 text-sm mt-0.5 ${width || 'w-full'}`} />
    </div>
  )
}
