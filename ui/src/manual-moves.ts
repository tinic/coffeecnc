// Power feed and saved-position moves in Manual. Each move is one MDI block in machine
// coordinates (G53). Lengths are millimetres, as everywhere in the protocol; the blocks carry
// the interpreter's current program units so that no G20/G21 state changes.

import type { AxisLetter, AxisStatus, LengthUnit, Status, Vec3 } from './protocol'
import { maxSavedPositions, type SavedPosition } from './operator-settings'
import { mmPerInch } from './units'

/** A power feed stops this far inside the soft limit; a Go to lifts Z to this far below Z max. */
export const limitMarginMm = 0.5
/** Machine position within this distance of a target counts as arrived. */
export const arrivalToleranceMm = 0.05

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
  if (status === null) return 'No status from the machine yet'
  const found: AxisStatus[] = []
  for (const name of axes) {
    const axis = axisStatus(status, name)
    if (axis === undefined) return `This machine has no ${letter(name)} axis`
    // Soft limits apply only to a homed axis, so they cannot keep an unhomed move inside.
    if (!axis.homed) return `Home ${letter(name)} first`
    if (!limitsKnown(axis)) return `${letter(name)} has no soft limits`
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
  if (context.probeLoaded === true) return { ok: false, reason: 'A touch probe is loaded' }
  const { feed, maxFeed } = request
  if (!Number.isFinite(feed) || feed <= 0) return { ok: false, reason: 'Enter a feed above zero' }
  if (feed > maxFeed) return { ok: false, reason: `The feed is above the machine maximum` }
  const { status, axes: [axis] } = movable
  if (axis === undefined) return { ok: false, reason: `This machine has no ${letter(request.axis)} axis` }
  const name = `${letter(request.axis)}${request.direction > 0 ? '+' : '−'}`
  const end = request.direction > 0 ? axis.max_limit - limitMarginMm : axis.min_limit + limitMarginMm
  const room = (end - axis.machine) * request.direction
  let target: number
  if (request.extent.kind === 'limit') {
    target = end
  } else {
    const distance = request.extent.distance
    if (!Number.isFinite(distance) || distance <= 0) return { ok: false, reason: 'Enter a distance above zero' }
    if (distance > room) {
      return { ok: false, reason: room > arrivalToleranceMm
        ? `${name} has ${number(room, 'mm')} mm before the soft limit`
        : `${letter(request.axis)} is at its soft limit` }
    }
    target = axis.machine + request.direction * distance
  }
  if (room <= arrivalToleranceMm) return { ok: false, reason: `${letter(request.axis)} is at its soft limit` }
  const unit = status.program_units
  return { ok: true, plan: {
    axis: request.axis, direction: request.direction, start: axis.machine, target, feed,
    text: `${unitWord(unit)} G90 G94 G53 G1 ${letter(request.axis)}${number(target, unit)} F${feedWord(feed, unit)}`,
  } }
}

function within(axis: AxisStatus, value: number): boolean {
  return value >= axis.min_limit && value <= axis.max_limit
}

/**
 * Z up to the safe height, then X and Y, then Z down to the saved Z only when `includeZ`. The
 * safe height is Z max less the margin, or the present Z when that is higher. Moves that would
 * not move are left out.
 */
export function goToPlan(context: MoveContext, saved: Vec3, includeZ: boolean): Planned<GoToPlan> {
  const movable = movableAxes(context, ['x', 'y', 'z'])
  if (typeof movable === 'string') return { ok: false, reason: movable }
  const { status, axes: [x, y, z] } = movable
  if (x === undefined || y === undefined || z === undefined) return { ok: false, reason: 'This machine has no X, Y and Z axes' }
  for (const [axis, value] of [[x, saved[0]], [y, saved[1]], ...(includeZ ? [[z, saved[2]] as const] : [])] as const) {
    if (!within(axis, value)) return { ok: false, reason: `The saved ${letter(axis.axis)} is outside the soft limits` }
  }
  const unit = status.program_units
  const prefix = `${unitWord(unit)} G90 G53 G0`
  const safeZ = Math.max(z.machine, z.max_limit - limitMarginMm)
  const moves: GoToMove[] = []
  if (safeZ - z.machine > arrivalToleranceMm) moves.push({ target: { z: safeZ }, text: `${prefix} Z${number(safeZ, unit)}` })
  if (Math.abs(saved[0] - x.machine) > arrivalToleranceMm || Math.abs(saved[1] - y.machine) > arrivalToleranceMm) {
    moves.push({ target: { x: saved[0], y: saved[1] }, text: `${prefix} X${number(saved[0], unit)} Y${number(saved[1], unit)}` })
  }
  if (includeZ && Math.abs(saved[2] - safeZ) > arrivalToleranceMm) {
    moves.push({ target: { z: saved[2] }, text: `${prefix} Z${number(saved[2], unit)}` })
  }
  return { ok: true, plan: { moves, safeZ } }
}

/** "Position N": the first N from the list length up that no saved position uses. */
export function nextPositionName(positions: readonly SavedPosition[]): string {
  const names = new Set(positions.map((entry) => entry.name))
  let index = positions.length + 1
  while (names.has(`Position ${String(index)}`)) index += 1
  return `Position ${String(index)}`
}

/** Why the present position cannot be saved, or null. */
export function saveBlock(status: Status | null, positions: readonly SavedPosition[]): string | null {
  if (positions.length >= maxSavedPositions) return `At most ${String(maxSavedPositions)} positions. Delete one first`
  if (status === null) return 'No status from the machine yet'
  for (const name of ['x', 'y', 'z'] as const) {
    const axis = axisStatus(status, name)
    if (axis === undefined) return `This machine has no ${letter(name)} axis`
    if (!axis.homed) return `Home ${letter(name)} first`
  }
  return null
}

export function machinePosition(status: Status): Vec3 {
  const at = (name: AxisLetter): number => axisStatus(status, name)?.machine ?? 0
  return [at('x'), at('y'), at('z')]
}

/** The axes of `target` are within the arrival tolerance in `status`. */
export function arrived(status: Status, target: GoToMove['target']): boolean {
  return (Object.entries(target) as [AxisLetter, number][]).every(([name, value]) => {
    const axis = axisStatus(status, name)
    return axis !== undefined && Math.abs(axis.machine - value) <= arrivalToleranceMm
  })
}
