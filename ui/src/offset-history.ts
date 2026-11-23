// Words and checks for the native offset history: what an entry changed, who changed it, and
// whether Restore can write its earlier values back.

import { formatTime, language, languageTags, t } from './i18n'
import type { HistoryEntry, HistoryField, HistorySource, LengthUnit } from './protocol'
import { formatLength, toolLabel, wcsName } from './units'

export type HistoryFilter = 'all' | 'offsets' | 'tools'

const sourceNames = {
  touch_off: 'probe.offsetHistory.touchOff',
  zero: 'probe.offsetHistory.zero',
  half: 'probe.offsetHistory.half',
  probe: 'probe.offsetHistory.probe',
  tool_setter: 'probe.offsetHistory.toolSetter',
  tool_touch_off: 'probe.offsetHistory.toolTouchOff',
  tool_table: 'probe.offsetHistory.toolTable',
  work_offset_table: 'probe.offsetHistory.workOffsetTable',
  program: 'probe.offsetHistory.program',
  restore: 'probe.offsetHistory.restore',
  outside: 'probe.offsetHistory.outside',
} as const satisfies Record<Exclude<HistorySource, 'mdi'>, string>

/** Who made the change, in the panel's language. */
export function sourceName(source: HistorySource): string {
  return source === 'mdi' ? 'MDI' : t(sourceNames[source])
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
    case 'rotation_xy': return t('probe.offsetHistory.rotation')
    case 'length': return t('probe.offsetHistory.length')
    case 'diameter': return t('probe.offsetHistory.diameter')
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
  if (entry.changes.some((change) => change.after === null)) return t('probe.offsetHistory.toolRemoved')
  if (entry.changes.some((change) => change.before === null)) return t('probe.offsetHistory.noEarlier')
  return null
}

/** Time of day, with the date when it is not today, in the panel's language. */
export function entryTime(time: number, now: number): string {
  const at = new Date(time)
  return at.toDateString() === new Date(now).toDateString()
    ? formatTime(time)
    : `${at.toLocaleDateString(languageTags[language.value], { month: 'short', day: 'numeric' })} ${formatTime(time)}`
}
