// The simulator's offset history: the native OffsetHistoryStore's rules on the simulated
// machine's state. It knows all nine work systems, not only the active one; it is kept for the
// page only.

import type { HistoryEntry, HistoryField, HistorySource, OffsetHistory, Tool, Vec3 } from '../protocol'

export const maxHistoryEntries = 200
const tolerance = 1e-6
const axisFields: readonly HistoryField[] = ['x', 'y', 'z']

export interface HistoryState {
  offsets: readonly Vec3[]
  rotations: readonly number[]
  g92: Vec3
  tools: readonly Tool[]
}

interface Seen {
  systems: [number, number, number, number][]
  g92: Vec3
  tools: Map<number, { length: number; diameter: number }>
}

function differs(a: number, b: number): boolean {
  return Math.abs(a - b) > tolerance
}

function snapshot(state: HistoryState): Seen {
  return {
    systems: state.offsets.map((offset, index) => [offset[0], offset[1], offset[2], state.rotations[index] ?? 0]),
    g92: [...state.g92],
    tools: new Map(state.tools.filter((tool) => tool.number > 0).map((tool) => [tool.number, { length: tool.length, diameter: tool.diameter }])),
  }
}

export class SimOffsetHistory {
  private entries: HistoryEntry[] = []
  private nextId = 1
  private revision = 1
  private seen: Seen | null = null

  /** Records what changed since the last call; true when an entry was added. */
  observe(state: HistoryState, source: HistorySource, restores: number | null, time: number): boolean {
    const now = snapshot(state)
    const before = this.seen
    this.seen = now
    if (before === null) return false
    const added: HistoryEntry[] = []
    const entry = (target: HistoryEntry['target'], number: number, changes: HistoryEntry['changes']): void => {
      if (changes.length > 0) added.push({ id: 0, time, source, target, number, changes, restores })
    }
    now.systems.forEach((values, index) => {
      const known = before.systems[index]
      if (known === undefined) return
      const fields: HistoryField[] = [...axisFields, 'rotation_xy']
      entry('wcs', index + 1, fields.flatMap((field, i) =>
        differs(known[i] ?? 0, values[i] ?? 0) ? [{ field, before: known[i] ?? 0, after: values[i] ?? 0 }] : []))
    })
    entry('g92', 0, axisFields.flatMap((field, i) =>
      differs(before.g92[i] ?? 0, now.g92[i] ?? 0) ? [{ field, before: before.g92[i] ?? 0, after: now.g92[i] ?? 0 }] : []))
    for (const [number, tool] of now.tools) {
      const known = before.tools.get(number)
      entry('tool', number, [
        ...(!known || differs(known.length, tool.length) ? [{ field: 'length' as const, before: known?.length ?? null, after: tool.length }] : []),
        ...(!known || differs(known.diameter, tool.diameter) ? [{ field: 'diameter' as const, before: known?.diameter ?? null, after: tool.diameter }] : []),
      ])
    }
    for (const [number, tool] of before.tools)
      if (!now.tools.has(number))
        entry('tool', number, [{ field: 'length', before: tool.length, after: null }, { field: 'diameter', before: tool.diameter, after: null }])
    for (const found of added) {
      found.id = this.nextId++
      this.entries.unshift(found)
    }
    if (added.length === 0) return false
    this.entries.length = Math.min(this.entries.length, maxHistoryEntries)
    this.revision += 1
    return true
  }

  entry(id: number): HistoryEntry | undefined {
    return this.entries.find((candidate) => candidate.id === id)
  }

  message(): OffsetHistory {
    return {
      type: 'offset_history',
      revision: this.revision,
      storage: 'session_only',
      reason: 'The browser simulator keeps the history for this page only.',
      entries: this.entries.map((entry) => ({ ...entry, changes: entry.changes.map((change) => ({ ...change })) })),
    }
  }
}
