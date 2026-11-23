// The simulator's alarm log: the native AlarmLog's rules, kept in browser storage. Each page load
// of the simulator is a run of its own; a page is sent what earlier runs recorded.

import type { AlarmEntry, AlarmLog, FromMachine, ToMachine } from '../protocol'

const storageKey = 'coffeecnc.sim.alarm-log'
const maxEntries = 500
const maxCommands = 256
const maxCommandBytes = 512

type Storage = Pick<globalThis.Storage, 'getItem' | 'setItem'>

function readEntries(storage: Storage): AlarmEntry[] {
  const parsed: unknown = JSON.parse(storage.getItem(storageKey) ?? '[]')
  if (!Array.isArray(parsed)) return []
  return parsed.filter((entry): entry is AlarmEntry => typeof entry === 'object' && entry !== null &&
    typeof (entry as AlarmEntry).session === 'number' && typeof (entry as AlarmEntry).text === 'string')
}

export class SimAlarmLog {
  private readonly entries: AlarmEntry[] = []
  private readonly session: number
  private readonly commands = new Map<number, Record<string, unknown>>()
  private saved = true

  constructor(private readonly storage: () => Storage) {
    try {
      this.entries = readEntries(storage())
    } catch {
      // A private window or damaged storage: this run only.
      this.saved = false
    }
    this.session = Math.max(0, ...this.entries.map((entry) => entry.session)) + 1
  }

  /** A command the panel sent, for the result it gets. */
  request(message: ToMachine): void {
    const { id, ...command } = message
    const text = JSON.stringify(command)
    this.commands.set(id, text.length > maxCommandBytes ? { type: command.type } : command)
    for (const key of this.commands.keys()) {
      if (this.commands.size <= maxCommands) break
      this.commands.delete(key)
    }
  }

  /** A message the simulator sent: operator messages and refusals are recorded. */
  observe(message: FromMachine, now: number): void {
    if (message.type === 'message') {
      this.add({ session: this.session, time: message.time, level: message.level, text: message.text, outcome: null, command: null })
    } else if (message.type === 'result') {
      const command = this.commands.get(message.id) ?? null
      this.commands.delete(message.id)
      if (message.outcome === 'accepted') return
      this.add({ session: this.session, time: now, level: 'error', text: message.reason, outcome: message.outcome, command })
    }
  }

  /** What earlier runs recorded. */
  view(): AlarmLog {
    return {
      type: 'alarm_log',
      storage: this.saved ? 'saved' : 'session_only',
      reason: this.saved ? '' : 'The browser does not keep the simulator\'s log.',
      session: this.session,
      skipped: 0,
      entries: this.entries.filter((entry) => entry.session < this.session),
    }
  }

  private add(entry: AlarmEntry): void {
    this.entries.push(entry)
    if (this.entries.length > maxEntries) this.entries.splice(0, this.entries.length - maxEntries)
    if (!this.saved) return
    try {
      this.storage().setItem(storageKey, JSON.stringify(this.entries))
    } catch {
      this.saved = false
    }
  }
}
