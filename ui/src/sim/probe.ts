import type { FixedSetter, ProbedPoint, ProbeDirection, SetterMeasured, SetterState, Tool, Vec3 } from '../protocol'
import { demoFixture, demoProbe, demoProbeTools, demoSetter, type ProbeBlock } from './probe-fixture'
import { toWork, type Modal } from './gcode'
import { heightGrid } from '../height-map'
import {
  arcAnglesValid, arcCentreUncertainty, arcMaxCentreUncertainty, arcMinMeasuredSpan, bossDiameterPlausible, circleThrough, contactSigma,
  measuredSpan,
} from '../arc'

type XY = [number, number]
type Lateral = Exclude<ProbeDirection, 'z_top'>
type Cycle = Exclude<FixedSetter, { action: 'status' }>
type Axis = 'X' | 'Y' | 'Z' | 'XY'
export interface ProbeMove { target: Vec3; feed?: number; contact?: boolean }
/** `failure` fails the cycle after its moves; with `partial` the result keeps the setter state (a stopped height map). */
export interface ProbePlan { moves: ProbeMove[]; finish: () => SetterState; failure?: string; partial?: boolean }
/** Machine travel, mm. */
export interface Travel { readonly min: Vec3; readonly max: Vec3 }

const { stock, slanted, gauge, pocket, slot, ridge, bore, boss, setter, top,
  boreRadius, bossRadius, stockHeight, recessDepth } = demoFixture
const rubyRadius = 2.02
const reach = 10
const margin = 3
const clearance = 5
const depth = 2
const searchTravel = demoSetter.material_travel_mm ?? 35
const latchBackoff = demoSetter.latch_backoff_mm ?? 1

const starts = {
  top: [135, 95, 8], left: [103, 92, -4], right: [197, 92, -4],
  front: [135, 63, -4], back: [135, 137, -4],
  'front-left': [115, 75, 8], 'front-right': [185, 75, 8],
  'back-left': [115, 125, 8], 'back-right': [185, 125, 8],
  'inside-front-left': [165, 81, -5], 'inside-front-right': [179, 81, -5],
  'inside-back-left': [165, 89, -5], 'inside-back-right': [179, 89, -5],
  pocket: [174, 83, -5], 'slot-x': [126, 110, -5], 'slot-y': [125, 108, -5],
  'ridge-x': [...ridge.centre, 8], 'ridge-y': [...ridge.centre, 8],
  bore: [155, 108, -4], boss: [235, 170, 8],
  angle: [230, 48, -4], 'angle-left': [208, 61, -4], ball: [285, 190, 8],
  setter: [55, 55, -22], measure: [55, 55, -22],
} satisfies Record<string, Vec3>

export function probeSceneStart(setup: string): { tool: number; position: Vec3; origin: number } {
  const entry = Object.entries(starts).find(([name]) => name === setup)
  const [name, [x, y, z]] = entry ?? ['top', starts.top]
  const tool = name === 'measure' ? 1 : demoProbe
  const length = tool === demoProbe ? 48 : 62.5
  return { tool, position: [x, y, top + length + z], origin: name === 'top' ? top + 2 : top }
}

const directions: Record<Lateral, XY> = {
  x_minus: [-1, 0], x_plus: [1, 0], y_minus: [0, -1], y_plus: [0, 1],
}

/** A G5x rotation in (-180, 180]: applying an edge angle cannot pass the ±360° limit. */
export function wrapped(degrees: number): number {
  const turn = degrees - 360 * Math.round(degrees / 360)
  return turn === -180 ? 180 : turn
}
function turn([x, y]: XY, degrees: number): XY {
  const a = degrees * Math.PI / 180
  return [x * Math.cos(a) - y * Math.sin(a), x * Math.sin(a) + y * Math.cos(a)]
}
function relative(point: Vec3, block: ProbeBlock): XY {
  return turn([point[0] - block.centre[0], point[1] - block.centre[1]], -block.angle)
}
function over(point: Vec3, block: ProbeBlock): boolean {
  const [x, y] = relative(point, block)
  return Math.abs(x) <= block.half[0] && Math.abs(y) <= block.half[1]
}
function radiusFrom(point: Vec3, centre: XY): number {
  return Math.hypot(point[0] - centre[0], point[1] - centre[1])
}
function along(point: Vec3, direction: XY, distance: number): Vec3 {
  return [point[0] + direction[0] * distance, point[1] + direction[1] * distance, point[2]]
}
function rayBox(start: Vec3, direction: XY, block: ProbeBlock): number | null {
  const point = relative(start, block)
  const vector = turn(direction, -block.angle)
  let entry = -Infinity, exit = Infinity
  for (const axis of [0, 1] as const) {
    const bound = block.half[axis] + rubyRadius
    const spread = vector[axis]
    if (Math.abs(spread) < 1e-9) {
      if (Math.abs(point[axis]) > block.half[axis]) return null
      continue
    }
    const a = (-bound - point[axis]) / spread
    const b = (bound - point[axis]) / spread
    entry = Math.max(entry, Math.min(a, b))
    exit = Math.min(exit, Math.max(a, b))
  }
  return entry > 0 && entry <= exit ? entry : null
}
function rayInnerWall(start: Vec3, direction: XY, recess: ProbeBlock): number | null {
  const point = relative(start, recess)
  const [hx, hy] = recess.half
  if (Math.abs(point[0]) >= hx - rubyRadius || Math.abs(point[1]) >= hy - rubyRadius) return null
  // A rotated work system probes the walls at an angle: the nearest wall the ray leaves through.
  const vector = turn(direction, -recess.angle)
  let exit = Infinity
  for (const axis of [0, 1] as const) {
    const spread = vector[axis]
    if (Math.abs(spread) < 1e-9) continue
    const wall = recess.half[axis] - rubyRadius
    exit = Math.min(exit, ((spread > 0 ? wall : -wall) - point[axis]) / spread)
  }
  return exit
}
function rayRound(start: Vec3, direction: XY, centre: XY, radius: number, inside: boolean): number | null {
  const x = start[0] - centre[0], y = start[1] - centre[1]
  const dot = x * direction[0] + y * direction[1]
  const chord = dot * dot - (x * x + y * y - radius * radius)
  if (chord < 0) return null
  const distance = -dot + (inside ? 1 : -1) * Math.sqrt(chord)
  return distance > 0 ? distance : null
}

class Path {
  readonly moves: ProbeMove[] = []
  /** Every search: where it began and the ball-centre contact. */
  readonly contacts: { from: Vec3; contact: Vec3 }[] = []
  constructor(readonly start: Vec3) {}
  move(target: Vec3): void { this.moves.push({ target }) }
  touch(from: Vec3, contact: Vec3): void {
    this.contacts.push({ from, contact })
    const length = Math.hypot(contact[0] - from[0], contact[1] - from[1], contact[2] - from[2])
    const retreat = Math.min(latchBackoff / length, 1)
    const backoff: Vec3 = [
      contact[0] + (from[0] - contact[0]) * retreat,
      contact[1] + (from[1] - contact[1]) * retreat,
      contact[2] + (from[2] - contact[2]) * retreat,
    ]
    this.moves.push({ target: contact, feed: demoSetter.feed_mm_min, contact: true })
    for (let pass = 0; pass < 3; pass++) {
      this.move(backoff)
      this.moves.push({ target: contact, feed: demoSetter.latch_feed_mm_min ?? 300, contact: true })
    }
    this.move(from)
  }
}

function lateral(path: Path, from: Vec3, direction: XY, distance: number | null): Vec3 | null {
  if (distance === null || distance > searchTravel) return null
  const contact = along(from, direction, distance)
  path.touch(from, contact)
  return contact
}

function outside(path: Path, gaugeZ: number, from: Vec3, direction: XY, distance: number,
  hit: (start: Vec3) => number | null): Vec3 | null {
  const at = along(from, direction, -distance)
  const high = Math.max(path.start[2], gaugeZ + clearance)
  path.move([at[0], at[1], high])
  at[2] = gaugeZ - rubyRadius - depth
  path.move(at)
  const contact = lateral(path, at, direction, hit(at))
  path.move([at[0], at[1], high])
  return contact
}

function frame(modal: Modal): string {
  return JSON.stringify([modal.wcs, modal.offsets[modal.wcs - 1], modal.rotations[modal.wcs - 1],
    modal.g92, modal.tool, modal.toolOffset])
}

export class SimProbe {
  private calibration: Vec3 | null = null
  private teacher = 0
  private readonly measured = new Map(demoProbeTools.map(tool => [tool.number, tool.length]))
  private readonly physicalLengths: Map<number, number>
  /** A measured edge: the G5x rotation applying it writes. */
  private angle: { id: number; rotation: number; frame: string } | null = null
  private angleId = 0
  /** A measure-only result: the machine point applying makes the origin of `axis`. */
  private feature: { id: number; point: Vec3; axis: Axis; frame: string } | null = null
  private featureId = 0

  constructor(tools: readonly Tool[], private readonly travel: Travel = { min: [-Infinity, -Infinity, -Infinity], max: [Infinity, Infinity, Infinity] }) {
    this.physicalLengths = new Map(tools.map(tool => [tool.number, tool.number === 1 ? 62.43 : tool.length]))
  }

  state(modal: Modal): SetterState {
    for (const [number, length] of this.measured) {
      if (modal.tools.find(tool => tool.number === number)?.length !== length) this.measured.delete(number)
    }
    if (!this.measured.has(demoSetter.reference_tool)) this.measured.clear()
    if (!this.measured.has(this.teacher)) this.calibration = null
    return { calibration: this.calibration, measurement: null, measured_tools: [...this.measured.keys()].sort((a, b) => a - b) }
  }

  plan(command: Cycle, start: Vec3, modal: Modal, tool: Tool): ProbePlan | string {
    const state = this.state(modal)
    const path = new Path(start)
    const length = this.physicalLengths.get(tool.number) ?? tool.length
    const gaugeZ = top + length
    const rotation = modal.rotations[modal.wcs - 1] ?? 0
    const tableRadius = tool.diameter / 2
    const applied = state.measured_tools?.includes(tool.number) && modal.toolOffset[2] === tool.length
    const noContact = 'No probe contact within the simulated search travel. Check the starting position.'
    const result = (update: () => Partial<SetterState>): ProbePlan => ({
      moves: path.moves,
      finish: (): SetterState => {
        const change = update()
        return { ...this.state(modal), ...change }
      },
    })
    const shift = (point: Vec3, axis: Axis): Partial<SetterState> => {
      const work = toWork(modal, point)
      const offset = modal.offsets[modal.wcs - 1]
      if (offset === undefined) throw new Error('Missing simulated work origin')
      const [x, y] = turn([axis === 'X' || axis === 'XY' ? work[0] : 0, axis === 'Y' || axis === 'XY' ? work[1] : 0], rotation)
      modal.offsets[modal.wcs - 1] = [offset[0] + x, offset[1] + y, offset[2] + (axis === 'Z' ? work[2] : 0)]
      return { material: { wcs: modal.wcs, axis } }
    }
    const measureOnly = 'measure_only' in command && command.measure_only
    // Each search's surface point: the ball centre one radius further along the travel.
    const faces = (): ProbedPoint[] => path.contacts.map(({ from, contact }) => {
      const length = Math.hypot(contact[0] - from[0], contact[1] - from[1])
      if (length < 1e-9) return { direction: 'z_top', work: toWork(modal, contact), machine: contact }
      const unit: XY = [(contact[0] - from[0]) / length, (contact[1] - from[1]) / length]
      const surface = along(contact, unit, tableRadius)
      const [u, v] = turn(unit, -rotation)
      const direction: Lateral = Math.abs(u) >= Math.abs(v) ? (u > 0 ? 'x_plus' : 'x_minus') : (v > 0 ? 'y_plus' : 'y_minus')
      return { direction, work: toWork(modal, surface), machine: surface }
    })
    // Measure-only: keep the point to apply later and report it; an XY point at the probing depth.
    const measure = (point: Vec3, axis: Axis): Partial<SetterState> => {
      const touched = faces()
      const at: Vec3 = axis === 'Z' ? point : [point[0], point[1], touched.at(-1)?.machine[2] ?? point[2]]
      const [x, y, z] = toWork(modal, at)
      const id = ++this.featureId
      this.feature = { id, point, axis, frame: frame(modal) }
      const work: SetterMeasured['work'] = {
        ...(axis.includes('X') ? { x } : {}), ...(axis.includes('Y') ? { y } : {}), ...(axis === 'Z' ? { z } : {}),
      }
      return { measured: { measurement_id: id, wcs: modal.wcs, axes: axis, work, machine: at, faces: touched } }
    }
    const origin = (point: Vec3, axis: Axis): ProbePlan => result(() => measureOnly ? measure(point, axis) : shift(point, axis))

    if (command.action === 'apply_measurement') {
      const measurement = this.feature
      this.feature = null
      this.angle = null
      if (measurement?.id !== command.measurement_id) return 'Probe the feature again before applying the measurement.'
      if (measurement.frame !== frame(modal)) return 'The work frame or tool changed. Probe the feature again.'
      return result(() => shift(measurement.point, measurement.axis))
    }
    this.feature = null
    if (command.action === 'apply_angle') {
      const measurement = this.angle
      this.angle = null
      if (measurement?.id !== command.measurement_id || measurement.frame !== frame(modal))
        return 'The work frame or tool changed. Measure the edge again.'
      return result(() => {
        modal.rotations[modal.wcs - 1] = measurement.rotation
        return { edge_angle: measurement.rotation }
      })
    }
    this.angle = null
    if (command.action === 'touch_off' || command.action === 'check') {
      const checking = command.action === 'check'
      if (checking && tool.number === demoProbe) return 'Load a cutting tool before checking its length.'
      const teaching = !checking && (tool.number === demoSetter.reference_tool ||
        (!state.calibration && state.measured_tools?.includes(tool.number)))
      if (!teaching && !state.calibration) return 'Teach the setter with T98 or a touched-off tool first.'
      if (checking && tool.length === 0 && !state.measured_tools?.includes(tool.number))
        return 'Set the loaded tool’s length before checking it.'
      if (radiusFrom(start, [setter[0], setter[1]]) > demoSetter.xy_tolerance_mm)
        return 'Jog directly over the centre of the simulated setter.'
      const contact: Vec3 = [start[0], start[1], setter[2] + length]
      if (start[2] <= contact[2] || start[2] - contact[2] > demoSetter.travel_mm) return noContact
      path.touch(start, contact)
      path.moves.pop()
      path.move([start[0], start[1], contact[2] + (demoSetter.clearance_mm ?? 0)])
      const previous = tool.length
      const value = teaching ? contact[2] - previous : contact[2] - (state.calibration?.[2] ?? setter[2])
      const measurement = {
        calibration: Boolean(teaching), tool: tool.number, contact, value, previous_length: previous, stopping_distance: 0.02,
      }
      if (checking) {
        const tolerance = demoSetter.check_tolerance_mm ?? 0.1
        const difference = value - previous
        if (Math.abs(difference) > tolerance) {
          const direction = difference < 0 ? 'shorter' : 'longer'
          return {
            ...result(() => ({})),
            failure: `T${String(tool.number)} check failed. Tool is ${Math.abs(difference).toFixed(4)} mm ${direction} ` +
              `(measured ${value.toFixed(4)} mm, table ${previous.toFixed(4)} mm, tolerance ${String(tolerance)} mm). Inspect the tool.`,
          }
        }
        return result(() => ({ measurement: { ...measurement, check_tolerance_mm: tolerance } }))
      }
      return result(() => {
        if (teaching) {
          this.calibration = [contact[0], contact[1], value]
          this.teacher = tool.number
        } else {
          tool.length = value
          modal.toolOffset = [0, 0, value]
          modal.lengthTool = tool.number
        }
        this.measured.set(tool.number, tool.length)
        return { calibration: this.calibration, measurement }
      })
    }
    if (tool.number !== demoProbe) return 'Load T99 to probe material.'
    if (command.action === 'height_map') {
      if (!applied) return 'Apply the measured T99 length before probing Z.'
      const grid = heightGrid([command.x_min, command.y_min, command.x_max, command.y_max], command.step)
      if (!grid) return 'Choose a rectangle and a step of 0.5 mm or more for 2 to 150 points.'
      // Work XYZ to machine XYZ, as toWork's inverse.
      const offset = modal.offsets[modal.wcs - 1] ?? [0, 0, 0]
      const toMachine = (x: number, y: number, z: number): Vec3 => {
        const [mx, my] = turn([x + modal.g92[0], y + modal.g92[1]], rotation)
        return [mx + offset[0] + modal.toolOffset[0], my + offset[1] + modal.toolOffset[1], z + offset[2] + modal.g92[2] + modal.toolOffset[2]]
      }
      const at = (index: number, count: number, from: number, to: number): number => count === 1 ? from : from + (to - from) * index / (count - 1)
      const where = (column: number, row: number): string =>
        `X${at(column, grid.columns, command.x_min, command.x_max).toFixed(3)} Y${at(row, grid.rows, command.y_min, command.y_max).toFixed(3)} mm`
      // As natively: every point's search start and end and the lift inside the travel, before any motion.
      const inTravel = (point: Vec3): boolean => point.every((value, axis) =>
        Number.isFinite(value) && value >= (this.travel.min[axis] ?? -Infinity) && value <= (this.travel.max[axis] ?? Infinity))
      const lift = Math.max(start[2], toMachine(command.x_min, command.y_min, command.clearance)[2])
      for (let row = 0; row < grid.rows; row++) {
        for (let column = 0; column < grid.columns; column++) {
          const [x, y, z] = toMachine(at(column, grid.columns, command.x_min, command.x_max), at(row, grid.rows, command.y_min, command.y_max), command.clearance)
          if (!inTravel([x, y, z]) || !inTravel([x, y, z - searchTravel]) || !inTravel([x, y, lift]))
            return `Grid point ${where(column, row)} is outside the configured travel at the clearance or the search depth.`
        }
      }
      const points: NonNullable<SetterState['height_map']>['points'] = Array<null>(grid.columns * grid.rows).fill(null)
      const map = (partial?: string): ProbePlan => result(() => ({
        height_map: { wcs: modal.wcs, columns: grid.columns, rows: grid.rows, rotation, points, ...(partial === undefined ? {} : { partial }) },
      }))
      // A stop after the first point keeps the points measured so far, as natively.
      const stop = (reason: string): ProbePlan | string =>
        points.some((point) => point !== null) ? { ...map(reason), failure: reason, partial: true } : reason
      let from = start
      for (let row = 0; row < grid.rows; row++) {
        for (let step = 0; step < grid.columns; step++) {
          const column = row % 2 === 0 ? step : grid.columns - 1 - step
          const above = toMachine(at(column, grid.columns, command.x_min, command.x_max), at(row, grid.rows, command.y_min, command.y_max), command.clearance)
          const high = Math.max(from[2], above[2])
          path.move([from[0], from[1], high])
          path.move([above[0], above[1], high])
          path.move(above)
          const solid = (over(above, stock) && radiusFrom(above, bore) > boreRadius) ||
            over(above, gauge) || over(above, ridge) || over(above, slanted) || radiusFrom(above, boss) <= bossRadius
          const surfaceZ = gaugeZ - (over(above, pocket) || over(above, slot) ? recessDepth : 0)
          if (!solid || above[2] <= surfaceZ || above[2] - surfaceZ > searchTravel) return stop(`Point ${where(column, row)}: ${noContact}`)
          const contact: Vec3 = [above[0], above[1], surfaceZ]
          path.touch(above, contact)
          points[row * grid.columns + column] = { work: toWork(modal, contact), machine: contact }
          from = above
          const gap = above[2] - surfaceZ
          if (gap < latchBackoff && points.some((point) => point === null)) {
            return stop(`The top at ${where(column, row)} is ${gap.toFixed(3)} mm below the clearance Z, within the ` +
              `${latchBackoff.toFixed(3)} mm latch back-off. Raise the clearance Z.`)
          }
        }
      }
      return map()
    }
    if (command.action === 'material') {
      if (command.direction === 'z_top') {
        if (!applied) return 'Touch off T99 and apply its length first.'
        const solid = (over(start, stock) && radiusFrom(start, bore) > boreRadius) ||
          over(start, gauge) || over(start, ridge) || over(start, slanted) || radiusFrom(start, boss) <= bossRadius
        const surfaceZ = gaugeZ - (over(start, pocket) || over(start, slot) ? recessDepth : 0)
        if (!solid || start[2] <= surfaceZ || start[2] - surfaceZ > searchTravel) return noContact
        const contact: Vec3 = [start[0], start[1], surfaceZ]
        path.touch(start, contact)
        return origin(contact, 'Z')
      }
      if (tableRadius <= 0) return 'Set the probe ball diameter first.'
      const base = directions[command.direction]
      const direction = turn(base, rotation)
      if (start[2] + rubyRadius >= gaugeZ || start[2] < gaugeZ - stockHeight) return noContact
      const distance = [stock, gauge, slanted, ridge].reduce<number | null>((nearest, block) => {
        const hit = rayBox(start, direction, block)
        return hit === null ? nearest : Math.min(nearest ?? Infinity, hit)
      }, null)
      const contact = lateral(path, start, direction, distance)
      return contact ? origin(along(contact, direction, tableRadius), base[0] === 0 ? 'Y' : 'X') : noContact
    }

    // Feature cycles run along the active work system's XY axes, turned by its rotation, and
    // compute their results in that frame: `toFrame` turns a machine point into it about the
    // machine origin, `fromFrame` turns it back, `axisOf` gives a work direction in machine XY.
    const toFrame = (point: Vec3): Vec3 => [...turn([point[0], point[1]], -rotation), point[2]]
    const fromFrame = (point: Vec3): Vec3 => [...turn([point[0], point[1]], rotation), point[2]]
    const axisOf = (direction: XY): XY => turn(direction, rotation)
    const startFrame = toFrame(start)
    if (command.action === 'angle') {
      const spacing = command.spacing
      if (!(Math.abs(spacing) >= 5 && Math.abs(spacing) <= 300)) return 'Enter a spacing of 5 to 300 mm.'
      if (start[2] + rubyRadius >= gaugeZ || start[2] < gaugeZ - stockHeight) return noContact
      const base = directions[command.direction]
      const direction = axisOf(base)
      const axis = base[0] === 0 ? 1 : 0
      const across = axis === 0 ? 1 : 0
      const nextFrame: Vec3 = [...startFrame]
      nextFrame[across] += spacing
      const next = fromFrame(nextFrame)
      const block = [slanted, stock, gauge].find(item => {
        const distance = rayBox(start, direction, item)
        return distance !== null && distance <= searchTravel
      })
      if (!block) return noContact
      const first = lateral(path, start, direction, rayBox(start, direction, block))
      path.move(next)
      const second = lateral(path, next, direction, rayBox(next, direction, block))
      if (!first || !second) return noContact
      const [a, b] = [toFrame(first), toFrame(second)]
      // The turn left from the work axis; applying it adds the turn to the present rotation.
      const degrees = (axis === 0 ? -1 : 1) * Math.atan((b[axis] - a[axis]) / (b[across] - a[across])) * 180 / Math.PI
      if (Math.abs(degrees) > 10) return 'Both contacts must be on the same straight edge.'
      return result(() => {
        const id = ++this.angleId
        this.angle = { id, rotation: wrapped(rotation + degrees), frame: frame(modal) }
        return {
          edge_angle: degrees, edge_rotation: { measurement_id: id, wcs: modal.wcs, rotation: wrapped(rotation + degrees) },
          ...(measureOnly ? { measured: { wcs: modal.wcs, axes: '', work: {}, faces: faces() } } : {}),
        }
      })
    }

    if (tableRadius <= 0) return 'Set the probe ball diameter first.'
    // Two frame faces as the machine point where the work system reads X0 Y0.
    const corner = (x: number, y: number): Vec3 => fromFrame([x, y, start[2]])
    if (command.action === 'bore_arc' || command.action === 'boss_arc') {
      if (!arcAnglesValid(command.angles))
        return 'Enter three probe angles at least 10° apart that cover at least 90° of the circle.'
      const inside = command.action === 'bore_arc'
      if (inside && (radiusFrom(start, bore) >= boreRadius - rubyRadius ||
          start[2] + rubyRadius >= gaugeZ || start[2] < gaugeZ - stockHeight)) return 'Jog the ball into the simulated bore below its top.'
      if (!inside) {
        if (!(command.diameter >= 2 && command.diameter <= 200)) return 'Enter a size from 2 to 200 mm.'
        if (!applied || Math.abs(toWork(modal, [start[0], start[1], gaugeZ])[2]) > 0.1 || start[2] <= gaugeZ)
          return 'Touch off T99, set Z0 on the feature top, and start above it.'
        if (radiusFrom(start, boss) >= bossRadius) return 'Start above the simulated boss.'
      }
      const contacts: [number, number][] = []
      for (const [index, degrees] of command.angles.entries()) {
        const heading = turn([1, 0], degrees)
        const contact = inside
          ? lateral(path, start, axisOf(heading), rayRound(start, axisOf(heading), bore, boreRadius - rubyRadius, true))
          : outside(path, gaugeZ, start, axisOf([-heading[0], -heading[1]]), command.diameter / 2 + reach + margin + rubyRadius,
            at => rayRound(at, axisOf([-heading[0], -heading[1]]), boss, bossRadius + rubyRadius, false))
        if (!contact) return `Contact ${String(index + 1)} at ${degrees.toFixed(1)}°: ${noContact}`
        const [u, v] = toFrame(contact)
        contacts.push([u, v])
      }
      const [a, b, c] = contacts as [[number, number], [number, number], [number, number]]
      const circle = circleThrough([a, b, c])
      const span = circle ? measuredSpan([a, b, c], circle.centre) : 0
      if (!circle || span < arcMinMeasuredSpan)
        return `The three contacts lie almost on a line: they cover ${span.toFixed(1)}° of a circle. Spread the angles further, and check that all three touched the same round wall.`
      const surface = circle.radius + (inside ? tableRadius : -tableRadius)
      if (!(surface > 0) || Math.hypot(startFrame[0] - circle.centre[0], startFrame[1] - circle.centre[1]) >= surface)
        return 'The circle through the contacts does not enclose the confirmed starting point.'
      if (!inside && !bossDiameterPlausible(2 * surface, command.diameter))
        return `The contacts give a boss of Ø ${(2 * surface).toFixed(3)} mm, which differs from the entered Ø ${command.diameter.toFixed(3)} mm by more than 0.5 mm or 10 %. Check that all three touched the boss, then probe again.`
      // The simulated latch repeats exactly: each contact counts with the 1 µm floor.
      const sigma = contactSigma(0)
      const uncertainty = arcCentreUncertainty([a, b, c], circle.centre, [sigma, sigma, sigma])
      if (uncertainty === null || uncertainty > arcMaxCentreUncertainty)
        return `The estimated centre uncertainty from latch repeatability is ${(uncertainty ?? Infinity).toFixed(3)} mm, more than 0.02 mm: the contacts cover ${span.toFixed(1)}° with a latch spread up to 0.0000 mm. Spread the angles further and probe again.`
      const offset = origin(corner(circle.centre[0], circle.centre[1]), 'XY')
      return { ...offset, finish: () => ({ ...offset.finish(), center_diameter: 2 * surface, arc_span: span, center_uncertainty: uncertainty }) }
    }
    if (command.action === 'bore') {
      if (radiusFrom(start, bore) >= boreRadius - rubyRadius ||
          start[2] + rubyRadius >= gaugeZ || start[2] < gaugeZ - stockHeight) return 'Jog the ball into the simulated bore below its top.'
      const hit = (from: Vec3, direction: XY): Vec3 | null => {
        const machine = axisOf(direction)
        return lateral(path, from, machine, rayRound(from, machine, bore, boreRadius - rubyRadius, true))
      }
      const right = hit(start, [1, 0]), left = hit(start, [-1, 0])
      if (!left || !right) return noContact
      const centreX = (toFrame(left)[0] + toFrame(right)[0]) / 2
      const centre = corner(centreX, startFrame[1])
      path.move(centre)
      const back = hit(centre, [0, 1]), front = hit(centre, [0, -1])
      if (!front || !back) return noContact
      const [backY, frontY] = [toFrame(back)[1], toFrame(front)[1]]
      const offset = origin(corner(centreX, (frontY + backY) / 2), 'XY')
      return { ...offset, finish: () => ({ ...offset.finish(), center_diameter: backY - frontY + tool.diameter }) }
    }

    if (command.action === 'inside_corner' || command.action === 'pocket' ||
        command.action === 'slot' || command.action === 'ridge') {
      const outsideRidge = command.action === 'ridge'
      const recess = command.action === 'slot' ? slot : pocket
      if (outsideRidge) {
        if (!(command.width >= 2 && command.width <= 200)) return 'Enter a width from 2 to 200 mm.'
        if (!applied || Math.abs(toWork(modal, [start[0], start[1], gaugeZ])[2]) > 0.1 || start[2] < gaugeZ + tableRadius || !over(start, ridge))
          return 'Touch off T99, set Z0 on the ridge top, and start above its middle.'
      } else if (!over(start, recess) || start[2] + rubyRadius >= gaugeZ || start[2] <= gaugeZ - recessDepth) {
        return 'Jog the ball into the simulated recess below its top and clear of the floor.'
      }
      const hit = (direction: XY): Vec3 | null => {
        const machine = axisOf(direction)
        return outsideRidge
          ? outside(path, gaugeZ, start, machine, command.width / 2 + tableRadius + margin + reach, at => rayBox(at, machine, ridge))
          : lateral(path, start, machine, rayInnerWall(start, machine, recess))
      }
      if (command.action === 'inside_corner') {
        const x = directions[command.x_direction], y = directions[command.y_direction]
        const a = hit(x), b = hit(y)
        if (!a || !b) return noContact
        return origin(corner(toFrame(a)[0] + x[0] * tableRadius, toFrame(b)[1] + y[1] * tableRadius), 'XY')
      }
      const across: Axis = command.action === 'pocket' ? 'XY' : command.axis === 'x' ? 'X' : 'Y'
      const point: Vec3 = [...startFrame]
      const size: NonNullable<SetterState['feature_size']> = {}
      for (const axis of [0, 1] as const) {
        const label = axis === 0 ? 'X' : 'Y'
        if (across !== 'XY' && across !== label) continue
        const forward: XY = axis === 0 ? [1, 0] : [0, 1]
        const backward: XY = [-forward[0], -forward[1]]
        const negative = hit(outsideRidge ? forward : backward)
        const positive = hit(outsideRidge ? backward : forward)
        if (!negative || !positive) return noContact
        const correction = outsideRidge ? tableRadius : -tableRadius
        const low = toFrame(negative)[axis] + correction
        const high = toFrame(positive)[axis] - correction
        if (!(low < startFrame[axis] && startFrame[axis] < high)) return 'The faces do not enclose the starting point.'
        point[axis] = (low + high) / 2
        size[axis === 0 ? 'x' : 'y'] = high - low
      }
      const offset = origin(fromFrame(point), across)
      return { ...offset, finish: () => ({ ...offset.finish(), feature_size: size }) }
    }

    if (!applied || Math.abs(toWork(modal, [start[0], start[1], gaugeZ])[2]) > 0.1 || start[2] <= gaugeZ)
      return 'Touch off T99, set Z0 on the feature top, and start above it.'
    const outsideDistance = reach + margin + rubyRadius
    if (command.action === 'corner') {
      const x = directions[command.x_direction], y = directions[command.y_direction]
      // The block's own axes: the rotation applied from its edge angle keeps them close.
      const block = [stock, slanted].find(item => over(start, item))
      const [sx, sy] = block ? relative(start, block) : [Infinity, Infinity]
      if (!block || block.half[0] + x[0] * sx > reach || block.half[1] + y[1] * sy > reach)
        return 'Start over the stock within 10 mm of both selected faces.'
      const [mx, my] = [axisOf(x), axisOf(y)]
      const a = outside(path, gaugeZ, start, mx, outsideDistance, at => rayBox(at, mx, block))
      const b = outside(path, gaugeZ, start, my, outsideDistance, at => rayBox(at, my, block))
      if (!a || !b) return noContact
      return origin(corner(toFrame(a)[0] + x[0] * tableRadius, toFrame(b)[1] + y[1] * tableRadius), 'XY')
    }
    const size = command.action === 'ball' ? command.gauge_width : command.diameter
    if (!(size >= 2 && size <= 200)) return 'Enter a size from 2 to 200 mm.'
    if (command.action === 'ball' && rotation !== 0) return 'Ball calibration needs a work system without rotation.'
    if (command.action === 'ball' && !over(start, gauge)) return 'Start above the simulated calibration gauge.'
    if (command.action === 'boss' && radiusFrom(start, boss) >= bossRadius) return 'Start above the simulated boss.'
    const hit = (from: Vec3, direction: XY): Vec3 | null => {
      const machine = axisOf(direction)
      return outside(path, gaugeZ, from, machine, size / 2 + outsideDistance, at => command.action === 'ball'
        ? rayBox(at, machine, gauge) : rayRound(at, machine, boss, bossRadius + rubyRadius, false))
    }
    const left = hit(start, [1, 0]), right = hit(start, [-1, 0])
    if (!left || !right) return noContact
    const [leftX, rightX] = [toFrame(left)[0], toFrame(right)[0]]
    if (command.action === 'ball') {
      const diameter = rightX - leftX - size
      if (diameter <= 0 || Math.abs(diameter - tool.diameter) > 0.5)
        return 'The measured ball diameter differs by more than 0.5 mm. Check the gauge width.'
      return result(() => {
        tool.diameter = Number(diameter.toFixed(6))
        return { ball_diameter: tool.diameter }
      })
    }
    const centreX = (leftX + rightX) / 2
    const centre = corner(centreX, startFrame[1])
    const front = hit(centre, [0, 1]), back = hit(centre, [0, -1])
    if (!front || !back) return noContact
    const [frontY, backY] = [toFrame(front)[1], toFrame(back)[1]]
    const offset = origin(corner(centreX, (frontY + backY) / 2), 'XY')
    return { ...offset, finish: () => ({ ...offset.finish(), center_diameter: backY - frontY - tool.diameter }) }
  }
}
