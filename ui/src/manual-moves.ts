// Power feed and saved-position moves in Manual. Each move is one MDI block in machine
// coordinates (G53). Lengths are millimetres, as everywhere in the protocol; the blocks carry
// the interpreter's current program units so that no G20/G21 state changes.

import type { AxisLetter, AxisStatus, LengthUnit, Status, Vec3 } from './protocol'
import { maxPositionNameBytes, maxSavedPositions, validPositionName, type SavedPosition } from './operator-settings'
import { t } from './i18n'
import { mmPerInch } from './units'

/** A power feed stops this far inside the soft limit; a Go to lifts Z to this far below Z max. */
export const limitMarginMm = 0.5
/** A target within this distance of the machine position needs no move. */
export const moveToleranceMm = 0.05
/**
 * The measured machine position within this distance of a commanded target counts as arrived.
 * LinuxCNC ends a G53 move exactly at the target; the position it reports comes from the
 * feedback, which on a machine with linear scales trails the command by the backlash (about
 * 0.125 mm on the G0759's Z) and drifts at rest. Only use the wider tolerance
 * when there is evidence the move has taken place.
 */
export const arrivalToleranceMm = 0.5

export type Planned<T> = { ok: true; plan: T } | { ok: false; reason: string }

export interface MoveContext {
  status: Status | null
  /** Why MDI is unavailable now (power, link, homing, a running program), or null. */
  machineBlock: string | null
  /** A touch probe is in the spindle. */
  probeLoaded?: boolean
}

export type FeedExtent = { kind: 'limit' } | { kind: 'distance'; distance: number }

export interface PowerFeedRequest {
  axis: AxisLetter
  direction: -1 | 1
  extent: FeedExtent
  /** mm/min. */
  feed: number
  /** mm/min; the fastest feed offered. */
  maxFeed: number
  /** The display unit, for lengths in the reason a feed is refused. */
  unit: LengthUnit
}

export interface PowerFeedPlan {
  axis: AxisLetter
  direction: -1 | 1
  start: number
  target: number
  feed: number
  text: string
}

export interface GoToMove {
  /** Axes this move commands, machine mm. */
  target: Partial<Record<AxisLetter, number>>
  text: string
}

export interface GoToPlan {
  moves: GoToMove[]
  /** The Z the X/Y move runs at. */
  safeZ: number
}

const letter = (axis: AxisLetter): string => axis.toUpperCase()

function number(mm: number, unit: LengthUnit): string {
  const decimals = unit === 'inch' ? 4 : 3
  const text = (unit === 'inch' ? mm / mmPerInch : mm).toFixed(decimals)
  return Number(text) === 0 ? (0).toFixed(decimals) : text
}

function feedWord(mmPerMin: number, unit: LengthUnit): string {
  return String(Number((unit === 'inch' ? mmPerMin / mmPerInch : mmPerMin).toFixed(unit === 'inch' ? 2 : 1)))
}

function unitWord(unit: LengthUnit): string {
  return unit === 'inch' ? 'G20' : 'G21'
}

function axisStatus(status: Status, axis: AxisLetter): AxisStatus | undefined {
  return status.axes.find((entry) => entry.axis === axis)
}

function limitsKnown(axis: AxisStatus): boolean {
  return Number.isFinite(axis.min_limit) && Number.isFinite(axis.max_limit) && axis.max_limit > axis.min_limit
}

/** The axes, in the order asked, when all of them may move under these features; else why not. */
function movableAxes(context: MoveContext, axes: readonly AxisLetter[]): { status: Status; axes: AxisStatus[] } | string {
  if (context.machineBlock !== null) return context.machineBlock
  const status = context.status
  if (status === null) return t('manual.moves.noStatus')
  const found: AxisStatus[] = []
  for (const name of axes) {
    const axis = axisStatus(status, name)
    if (axis === undefined) return t('manual.moves.noAxis', { axis: letter(name) })
    // Soft limits apply only to a homed axis, so they cannot keep an unhomed move inside.
    if (!axis.homed) return t('manual.moves.homeFirst', { axis: letter(name) })
    if (!limitsKnown(axis)) return t('manual.moves.noSoftLimits', { axis: letter(name) })
    found.push(axis)
  }
  return { status, axes: found }
}

/** The first reason none of `axes` may move under these features, or null. */
export function moveBlock(context: MoveContext, axes: readonly AxisLetter[]): string | null {
  const movable = movableAxes(context, axes)
  return typeof movable === 'string' ? movable : null
}

export function powerFeedPlan(context: MoveContext, request: PowerFeedRequest): Planned<PowerFeedPlan> {
  const movable = movableAxes(context, [request.axis])
  if (typeof movable === 'string') return { ok: false, reason: movable }
  if (context.probeLoaded === true) return { ok: false, reason: t('manual.moves.probeLoaded') }
  const { feed, maxFeed } = request
  if (!Number.isFinite(feed) || feed <= 0) return { ok: false, reason: t('manual.moves.enterFeed') }
  if (feed > maxFeed) return { ok: false, reason: t('manual.moves.feedAboveMax') }
  const { status, axes: [axis] } = movable
  if (axis === undefined) return { ok: false, reason: t('manual.moves.noAxis', { axis: letter(request.axis) }) }
  const name = `${letter(request.axis)}${request.direction > 0 ? '+' : '−'}`
  const end = request.direction > 0 ? axis.max_limit - limitMarginMm : axis.min_limit + limitMarginMm
  const room = (end - axis.machine) * request.direction
  let target: number
  if (request.extent.kind === 'limit') {
    target = end
  } else {
    const distance = request.extent.distance
    if (!Number.isFinite(distance) || distance <= 0) return { ok: false, reason: t('manual.moves.enterDistance') }
    if (distance > room) {
      return { ok: false, reason: room > moveToleranceMm
        ? t('manual.moves.room', { direction: name, room: number(room, request.unit), unit: request.unit === 'inch' ? 'in' : 'mm' })
        : t('manual.moves.atLimit', { axis: letter(request.axis) }) }
    }
    target = axis.machine + request.direction * distance
  }
  if (room <= moveToleranceMm) return { ok: false, reason: t('manual.moves.atLimit', { axis: letter(request.axis) }) }
  const unit = status.program_units
  return { ok: true, plan: {
    axis: request.axis, direction: request.direction, start: axis.machine, target, feed,
    text: `${unitWord(unit)} G90 G94 G53 G1 ${letter(request.axis)}${number(target, unit)} F${feedWord(feed, unit)}`,
  } }
}

function within(axis: AxisStatus, value: number): boolean {
  return value >= axis.min_limit && value <= axis.max_limit
}

/** Why `safeZ` (machine Z, mm) cannot be the safe height of Go to on this machine, or null. */
export function safeZBlock(status: Status | null, safeZ: number, unit: LengthUnit): string | null {
  const z = status === null ? undefined : axisStatus(status, 'z')
  if (z === undefined || !limitsKnown(z)) return t('manual.moves.zNoSoftLimits')
  if (!Number.isFinite(safeZ) || !within(z, safeZ)) {
    return t('manual.moves.safeZRange', { min: number(z.min_limit, unit), max: number(z.max_limit, unit), unit: unit === 'inch' ? 'in' : 'mm' })
  }
  return null
}

/**
 * Z up to the safe height, then X and Y, then Z down to the saved Z only when `includeZ`. The
 * safe height is `safeZSetting` (the operator setting, machine Z) or, when that is null, Z max
 * less the margin; the present Z when that is higher. Moves that would not move are left out.
 */
export function goToPlan(context: MoveContext, saved: Vec3, includeZ: boolean, safeZSetting: number | null = null): Planned<GoToPlan> {
  const movable = movableAxes(context, ['x', 'y', 'z'])
  if (typeof movable === 'string') return { ok: false, reason: movable }
  const { status, axes: [x, y, z] } = movable
  if (x === undefined || y === undefined || z === undefined) return { ok: false, reason: t('manual.moves.noXyz') }
  for (const [axis, value] of [[x, saved[0]], [y, saved[1]], ...(includeZ ? [[z, saved[2]] as const] : [])] as const) {
    if (!within(axis, value)) return { ok: false, reason: t('manual.moves.savedOutside', { axis: letter(axis.axis) }) }
  }
  const unit = status.program_units
  const prefix = `${unitWord(unit)} G90 G53 G0`
  if (safeZSetting !== null && !within(z, safeZSetting)) {
    return { ok: false, reason: t('manual.moves.safeZOutside') }
  }
  const safeZ = Math.max(z.machine, safeZSetting ?? z.max_limit - limitMarginMm)
  const moves: GoToMove[] = []
  if (safeZ - z.machine > moveToleranceMm) moves.push({ target: { z: safeZ }, text: `${prefix} Z${number(safeZ, unit)}` })
  if (Math.abs(saved[0] - x.machine) > moveToleranceMm || Math.abs(saved[1] - y.machine) > moveToleranceMm) {
    moves.push({ target: { x: saved[0], y: saved[1] }, text: `${prefix} X${number(saved[0], unit)} Y${number(saved[1], unit)}` })
  }
  if (includeZ && Math.abs(saved[2] - safeZ) > moveToleranceMm) {
    moves.push({ target: { z: saved[2] }, text: `${prefix} Z${number(saved[2], unit)}` })
  }
  return { ok: true, plan: { moves, safeZ } }
}

/** "Position N": the first N from the list length up that no saved position uses. */
export function nextPositionName(positions: readonly SavedPosition[]): string {
  const names = new Set(positions.map((entry) => entry.name))
  let index = positions.length + 1
  while (names.has(t('manual.moves.defaultName', { n: index }))) index += 1
  return t('manual.moves.defaultName', { n: index })
}

/**
 * The name typed for a new or renamed position: trimmed, `fallback` when empty. Refused when
 * longer than the settings keep, or when another saved position (not `renaming`) has it, so
 * that every row and every Go to names one position.
 */
export function chosenPositionName(text: string, fallback: string, positions: readonly SavedPosition[],
  renaming: SavedPosition | null = null): Planned<string> {
  const name = text.trim() || fallback
  if (!validPositionName(name)) return { ok: false, reason: t('manual.moves.nameTooLong', { bytes: maxPositionNameBytes }) }
  if (positions.some((entry) => entry !== renaming && entry.name === name)) return { ok: false, reason: t('manual.moves.nameTaken', { name }) }
  return { ok: true, plan: name }
}

/** Why the present position cannot be saved, or null. */
export function saveBlock(status: Status | null, positions: readonly SavedPosition[]): string | null {
  if (positions.length >= maxSavedPositions) return t('manual.moves.tooMany', { count: maxSavedPositions })
  if (status === null) return t('manual.moves.noStatus')
  for (const name of ['x', 'y', 'z'] as const) {
    const axis = axisStatus(status, name)
    if (axis === undefined) return t('manual.moves.noAxis', { axis: letter(name) })
    if (!axis.homed) return t('manual.moves.homeFirst', { axis: letter(name) })
  }
  return null
}

export function machinePosition(status: Status): Vec3 {
  const at = (name: AxisLetter): number => axisStatus(status, name)?.machine ?? 0
  return [at('x'), at('y'), at('z')]
}

/** The axes of `target` are within the arrival tolerance in `status`. */
export function arrived(status: Status, target: GoToMove['target'], tolerance = arrivalToleranceMm): boolean {
  return (Object.entries(target) as [AxisLetter, number][]).every(([name, value]) => {
    const axis = axisStatus(status, name)
    return axis !== undefined && Math.abs(axis.machine - value) <= tolerance
  })
}
