export function formatDuration(seconds: number): string {
  if (!seconds) return '0s'
  if (seconds < 60) return `${Math.round(seconds)}s`
  const h = Math.floor(seconds / 3600)
  const m = Math.floor((seconds % 3600) / 60)
  const s = Math.round(seconds % 60)
  if (h > 0) return m > 0 ? `${h}h ${m}m` : `${h}h`
  return s > 0 ? `${m}m ${s}s` : `${m}m`
}

export function formatTimestamp(ts: string): string {
  try {
    const d = new Date(ts)
    return d.toLocaleDateString('en-US', { month: 'short', day: 'numeric' }) +
      ' ' + d.toLocaleTimeString('en-US', { hour: '2-digit', minute: '2-digit', hour12: false })
  } catch {
    return ts
  }
}

export function shortDeviceId(id: string): string {
  if (!id) return '\u2014'
  return id.slice(0, 8) + '...'
}

export function titleCase(snakeId: string): string {
  return snakeId.split('_').map(w => w.charAt(0).toUpperCase() + w.slice(1)).join(' ')
}
