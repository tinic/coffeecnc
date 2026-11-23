// What the diagnostics view makes of a HAL reading: the rows grouped as the view shows them, and
// for each row the words and the tone of its value. Pure: the view translates the keys.

import type { DiagnosticInput, DiagnosticRole, Diagnostics } from './protocol'

/** `go` is the safe or ready state, `stop` a tripped limit, a fault or an open E-stop chain. */
export type Tone = 'go' | 'stop' | 'warn' | 'active' | 'idle'

export interface Shown {
  /** A key of status.diagnostics.value, or null for a number shown as it is (`text`). */
  key: string | null
  /** The number, formatted, when `key` is null. */
  text: string
  tone: Tone
  /** A key of status.diagnostics.state when the row is not live: its value means little. */
  note: string | null
}

/** Words of a bit by role: [true, false], each with its tone. */
const bits: Record<Exclude<DiagnosticRole, 'spindle_speed_out' | 'spindle_speed_in'>, readonly [[string, Tone], [string, Tone]]> = {
  min_limit: [['status.diagnostics.value.tripped', 'stop'], ['status.diagnostics.value.clear', 'go']],
  max_limit: [['status.diagnostics.value.tripped', 'stop'], ['status.diagnostics.value.clear', 'go']],
  home_switch: [['status.diagnostics.value.on', 'active'], ['status.diagnostics.value.off', 'idle']],
  amp_fault: [['status.diagnostics.value.fault', 'stop'], ['status.diagnostics.value.ok', 'go']],
  estop_chain: [['status.diagnostics.value.closed', 'go'], ['status.diagnostics.value.open', 'stop']],
  estop_request: [['status.diagnostics.value.enabled', 'go'], ['status.diagnostics.value.estop', 'stop']],
  probe: [['status.diagnostics.value.tripped', 'warn'], ['status.diagnostics.value.clear', 'go']],
  spindle_on: [['status.diagnostics.value.on', 'active'], ['status.diagnostics.value.off', 'idle']],
  spindle_at_speed: [['status.diagnostics.value.atSpeed', 'go'], ['status.diagnostics.value.notAtSpeed', 'idle']],
  spindle_fault: [['status.diagnostics.value.fault', 'stop'], ['status.diagnostics.value.ok', 'go']],
  configured: [['status.diagnostics.value.on', 'active'], ['status.diagnostics.value.off', 'idle']],
}

const notes = {
  missing: 'status.diagnostics.state.missing',
  unwired: 'status.diagnostics.state.unwired',
  undriven: 'status.diagnostics.state.undriven',
  live: null,
} as const
/** Why a row that is not live means little, for its tooltip. */
export const stateTips = {
  missing: 'status.diagnostics.stateTip.missing',
  unwired: 'status.diagnostics.stateTip.unwired',
  undriven: 'status.diagnostics.stateTip.undriven',
  live: null,
} as const

function number(value: number, digits: number): string {
  return Number.isInteger(value) && digits === 0 ? String(value) : String(Number(value.toFixed(digits)))
}

/** The value of a row as the view shows it. */
export function shown(input: DiagnosticInput): Shown {
  const note = notes[input.state]
  const value = input.value
  if (input.state === 'missing' || value === null) return { key: 'status.diagnostics.value.none', text: '', tone: 'idle', note }
  if (typeof value === 'number') {
    // speed-in is revolutions per second; both speeds are shown in rpm.
    const rpm = input.role === 'spindle_speed_in' ? value * 60 : input.role === 'spindle_speed_out' ? value : null
    const text = rpm === null ? number(value, 3) : `${number(rpm, 0)} rpm` // i18n-ignore: a unit
    return { key: null, text, tone: input.state === 'live' ? 'active' : 'idle', note }
  }
  const role = input.role === 'spindle_speed_in' || input.role === 'spindle_speed_out' ? 'configured' : input.role
  const [word, tone] = bits[role][value ? 0 : 1]
  // A value nothing drives is not a reading of a switch: never shown as a fault or as safe.
  return { key: word, text: '', tone: input.state === 'live' ? tone : 'idle', note }
}

export interface JointRow {
  joint: number
  axis: string | null
  min: DiagnosticInput | null
  max: DiagnosticInput | null
  home: DiagnosticInput | null
  fault: DiagnosticInput | null
}

export interface Groups {
  joints: JointRow[]
  estop: DiagnosticInput[]
  probe: DiagnosticInput[]
  spindle: DiagnosticInput[]
  configured: DiagnosticInput[]
}

const spindleRoles: readonly DiagnosticRole[] = ['spindle_on', 'spindle_at_speed', 'spindle_speed_out', 'spindle_speed_in', 'spindle_fault']

/** The rows of a reading by section, in the order they came. */
export function groups(diagnostics: Diagnostics | null): Groups {
  const result: Groups = { joints: [], estop: [], probe: [], spindle: [], configured: [] }
  for (const input of diagnostics?.inputs ?? []) {
    if (input.joint !== undefined) {
      let row = result.joints.find((candidate) => candidate.joint === input.joint)
      if (row === undefined) {
        row = { joint: input.joint, axis: input.axis ?? null, min: null, max: null, home: null, fault: null }
        result.joints.push(row)
      }
      if (input.role === 'min_limit') row.min = input
      else if (input.role === 'max_limit') row.max = input
      else if (input.role === 'home_switch') row.home = input
      else if (input.role === 'amp_fault') row.fault = input
    } else if (input.role === 'estop_chain' || input.role === 'estop_request') {
      result.estop.push(input)
    } else if (input.role === 'probe') {
      result.probe.push(input)
    } else if (spindleRoles.includes(input.role)) {
      result.spindle.push(input)
    } else {
      result.configured.push(input)
    }
  }
  return result
}

/** Something needs the operator's eye: a tripped limit, a fault, or the E-stop chain open. */
export function alarms(diagnostics: Diagnostics | null): number {
  return (diagnostics?.inputs ?? []).filter((input) => shown(input).tone === 'stop').length
}
