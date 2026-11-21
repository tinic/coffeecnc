// Words and checks for the native offset history: what an entry changed, who changed it, and
// whether Restore can write its earlier values back.

import type { HistoryEntry, HistoryField, HistorySource, LengthUnit } from './protocol'
import { formatClock, formatLength, toolLabel, wcsName } from './units'

export type HistoryFilter = 'all' | 'offsets' | 'tools'

const sourceNames: Record<HistorySource, string> = {
  touch_off: 'Touch-off',
  zero: 'Zero',
  half: 'Half',
  probe: 'Probe',
  tool_setter: 'Tool setter',
  tool_touch_off: 'Tool length touch-off',
  tool_table: 'Tool table edit',
  work_offset_table: 'Work offset edit',
  mdi: 'MDI',
  program: 'Program',
  restore: 'Restore',
  outside: 'Outside the panel',
}

export function sourceName(source: HistorySource): string {
  return sourceNames[source]
}

/** G54, G92/G52 or T3. */
export function targetName(entry: HistoryEntry): string {
  if (entry.target === 'wcs') return wcsName(entry.number)
  if (entry.target === 'g92') return 'G92/G52'
  return toolLabel(entry.number)
}

export function fieldName(field: HistoryField): string {
  switch (field) {
    case 'x': return 'X'
    case 'y': return 'Y'
    case 'z': return 'Z'
    case 'rotation_xy': return 'Rotation'
    case 'length': return 'Length'
    case 'diameter': return 'Diameter'
  }
}

/** The axis of an X, Y or Z field; null for the others. */
export function fieldAxis(field: HistoryField): 0 | 1 | 2 | null {
  return field === 'x' ? 0 : field === 'y' ? 1 : field === 'z' ? 2 : null
}

/** A value of the field in the display unit; rotations in degrees. */
export function fieldValue(field: HistoryField, value: number | null, unit: LengthUnit): string {
  if (value === null) return '—'
  if (field === 'rotation_xy') return `${String(Number(value.toFixed(4)))}°`
  return formatLength(value, unit)
}

export function matchesFilter(entry: HistoryEntry, filter: HistoryFilter): boolean {
  return filter === 'all' || (filter === 'tools') === (entry.target === 'tool')
}

/** Why Restore is not offered for this entry, or null. */
export function restoreBlock(entry: HistoryEntry): string | null {
  if (entry.changes.some((change) => change.after === null)) return 'The tool was removed'
  if (entry.changes.some((change) => change.before === null)) return 'No earlier value is known'
  return null
}

/** Time of day, with the date when it is not today. */
export function entryTime(time: number, now: number): string {
  const at = new Date(time)
  return at.toDateString() === new Date(now).toDateString()
    ? formatClock(time)
    : `${at.toLocaleDateString(undefined, { month: 'short', day: 'numeric' })} ${formatClock(time)}`
}
