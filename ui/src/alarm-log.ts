// The alarm log of earlier runs and earlier pages, as the message log shows it: one group per
// run of CoffeeCNC, newest first, each with its entries newest first.

import type { AlarmEntry, AlarmLog, CommandBody } from './protocol'
import { describeCommand } from './store'

export interface AlarmGroup {
  session: number
  /** This run of CoffeeCNC: its entries are from before this page was loaded. */
  current: boolean
  /** The time of the group's first entry, milliseconds since the Unix epoch. */
  since: number
  entries: AlarmEntry[]
}

export function alarmGroups(log: AlarmLog | null): AlarmGroup[] {
  if (log === null) return []
  const groups: AlarmGroup[] = []
  // The log is in the order it was written; a run's entries are together.
  for (const entry of log.entries) {
    const last = groups.at(-1)
    if (last?.session === entry.session) last.entries.push(entry)
    else groups.push({ session: entry.session, current: entry.session === log.session, since: entry.time, entries: [entry] })
  }
  for (const group of groups) group.entries.reverse()
  return groups.reverse()
}

// Undefined for a command type this panel does not know.
const describe: (command: CommandBody) => string | undefined = describeCommand

/** The words of the log for an entry's command; empty when there is none or it cannot be read. */
export function describeAlarmCommand(command: AlarmEntry['command']): string {
  if (command === null) return ''
  const type = typeof command['type'] === 'string' ? command['type'] : ''
  try {
    return describe(command as unknown as CommandBody) ?? type
  } catch {
    // Written by another panel version: a field this one expects is missing.
    return type
  }
}
