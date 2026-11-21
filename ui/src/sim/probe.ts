import type { FixedSetter, ProbeDirection, SetterState, Tool, Vec3 } from '../protocol'
import { demoFixture, demoProbe, demoProbeTools, demoSetter, type ProbeBlock } from './probe-fixture'
import { toWork, type Modal } from './gcode'

type XY = [number, number]
type Lateral = Exclude<ProbeDirection, 'z_top'>
type Cycle = Exclude<FixedSetter, { action: 'status' }>
type Axis = 'X' | 'Y' | 'Z' | 'XY'
export interface ProbeMove { target: Vec3; feed?: number; contact?: boolean }
export interface ProbePlan { moves: ProbeMove[]; finish: () => SetterState; failure?: string }

const { stock, slanted, gauge, bore, boss, setter, top, boreRadius, bossRadius, stockHeight } = demoFixture
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
  constructor(readonly start: Vec3) {}
  move(target: Vec3): void { this.moves.push({ target }) }
  touch(from: Vec3, contact: Vec3): void {
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
  private angle: { id: number; degrees: number; frame: string } | null = null
  private angleId = 0

  constructor(tools: readonly Tool[]) {
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
    const origin = (point: Vec3, axis: Axis): ProbePlan => result(() => {
      const work = toWork(modal, point)
      const offset = modal.offsets[modal.wcs - 1]
      if (offset === undefined) throw new Error('Missing simulated work origin')
      const [x, y] = turn([axis === 'X' || axis === 'XY' ? work[0] : 0, axis === 'Y' || axis === 'XY' ? work[1] : 0], rotation)
      modal.offsets[modal.wcs - 1] = [offset[0] + x, offset[1] + y, offset[2] + (axis === 'Z' ? work[2] : 0)]
      return { material: { wcs: modal.wcs, axis } }
    })

    if (command.action === 'apply_angle') {
      const measurement = this.angle
      this.angle = null
      if (measurement?.id !== command.measurement_id || measurement.frame !== frame(modal))
        return 'The work frame or tool changed. Measure the edge again.'
      return result(() => {
        modal.rotations[modal.wcs - 1] = measurement.degrees
        return { edge_angle: measurement.degrees }
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
        }
        this.measured.set(tool.number, tool.length)
        return { calibration: this.calibration, measurement }
      })
    }
    if (tool.number !== demoProbe) return 'Load T99 to probe material.'
    if (command.action === 'material') {
      if (command.direction === 'z_top') {
        if (!applied) return 'Touch off T99 and apply its length first.'
        const solid = (over(start, stock) && radiusFrom(start, bore) > boreRadius) ||
          over(start, gauge) || over(start, slanted) || radiusFrom(start, boss) <= bossRadius
        if (!solid || start[2] <= gaugeZ || start[2] - gaugeZ > searchTravel) return noContact
        const contact: Vec3 = [start[0], start[1], gaugeZ]
        path.touch(start, contact)
        return origin(contact, 'Z')
      }
      if (tableRadius <= 0) return 'Set the probe ball diameter first.'
      const base = directions[command.direction]
      const direction = turn(base, rotation)
      if (start[2] + rubyRadius >= gaugeZ || start[2] < gaugeZ - stockHeight) return noContact
      const distance = [stock, gauge, slanted].reduce<number | null>((nearest, block) => {
        const hit = rayBox(start, direction, block)
        return hit === null ? nearest : Math.min(nearest ?? Infinity, hit)
      }, null)
      const contact = lateral(path, start, direction, distance)
      return contact ? origin(along(contact, direction, tableRadius), base[0] === 0 ? 'Y' : 'X') : noContact
    }

    if (rotation !== 0) return 'Use a work coordinate system without rotation for this probe cycle.'
    if (command.action === 'angle') {
      const spacing = command.spacing
      if (!(Math.abs(spacing) >= 5 && Math.abs(spacing) <= 300)) return 'Enter a spacing of 5 to 300 mm.'
      if (start[2] + rubyRadius >= gaugeZ || start[2] < gaugeZ - stockHeight) return noContact
      const direction = directions[command.direction]
      const axis = direction[0] === 0 ? 1 : 0
      const next: Vec3 = [...start]
      next[axis === 0 ? 1 : 0] += spacing
      const block = [slanted, stock, gauge].find(item => {
        const distance = rayBox(start, direction, item)
        return distance !== null && distance <= searchTravel
      })
      if (!block) return noContact
      const first = lateral(path, start, direction, rayBox(start, direction, block))
      path.move(next)
      const second = lateral(path, next, direction, rayBox(next, direction, block))
      if (!first || !second) return noContact
      const degrees = (axis === 0 ? -1 : 1) * Math.atan((second[axis] - first[axis]) / spacing) * 180 / Math.PI
      if (Math.abs(degrees) > 10) return 'Both contacts must be on the same straight edge.'
      return result(() => {
        const id = ++this.angleId
        this.angle = { id, degrees, frame: frame(modal) }
        return { edge_angle: degrees, edge_rotation: { measurement_id: id, wcs: modal.wcs } }
      })
    }

    if (tableRadius <= 0) return 'Set the probe ball diameter first.'
    if (command.action === 'bore') {
      if (radiusFrom(start, bore) >= boreRadius - rubyRadius ||
          start[2] + rubyRadius >= gaugeZ || start[2] < gaugeZ - stockHeight) return 'Jog the ball into the simulated bore below its top.'
      const hit = (from: Vec3, direction: XY): Vec3 | null =>
        lateral(path, from, direction, rayRound(from, direction, bore, boreRadius - rubyRadius, true))
      const right = hit(start, [1, 0]), left = hit(start, [-1, 0])
      if (!left || !right) return noContact
      const centre: Vec3 = [(left[0] + right[0]) / 2, start[1], start[2]]
      path.move(centre)
      const back = hit(centre, [0, 1]), front = hit(centre, [0, -1])
      if (!front || !back) return noContact
      const offset = origin([centre[0], (front[1] + back[1]) / 2, centre[2]], 'XY')
      return { ...offset, finish: () => ({ ...offset.finish(), center_diameter: back[1] - front[1] + tool.diameter }) }
    }

    if (!applied || Math.abs(toWork(modal, [start[0], start[1], gaugeZ])[2]) > 0.1 || start[2] <= gaugeZ)
      return 'Touch off T99, set Z0 on the feature top, and start above it.'
    const outsideDistance = reach + margin + rubyRadius
    if (command.action === 'corner') {
      const x = directions[command.x_direction], y = directions[command.y_direction]
      const [sx, sy] = relative(start, stock)
      if (!over(start, stock) || stock.half[0] + x[0] * sx > reach || stock.half[1] + y[1] * sy > reach)
        return 'Start over the stock within 10 mm of both selected faces.'
      const a = outside(path, gaugeZ, start, x, outsideDistance, at => rayBox(at, x, stock))
      const b = outside(path, gaugeZ, start, y, outsideDistance, at => rayBox(at, y, stock))
      if (!a || !b) return noContact
      return origin([a[0] + x[0] * tableRadius, b[1] + y[1] * tableRadius, start[2]], 'XY')
    }
    const size = command.action === 'ball' ? command.gauge_width : command.diameter
    if (!(size >= 2 && size <= 200)) return 'Enter a size from 2 to 200 mm.'
    if (command.action === 'ball' && !over(start, gauge)) return 'Start above the simulated calibration gauge.'
    if (command.action === 'boss' && radiusFrom(start, boss) >= bossRadius) return 'Start above the simulated boss.'
    const hit = (from: Vec3, direction: XY): Vec3 | null =>
      outside(path, gaugeZ, from, direction, size / 2 + outsideDistance, at => command.action === 'ball'
        ? rayBox(at, direction, gauge) : rayRound(at, direction, boss, bossRadius + rubyRadius, false))
    const left = hit(start, [1, 0]), right = hit(start, [-1, 0])
    if (!left || !right) return noContact
    if (command.action === 'ball') {
      const diameter = right[0] - left[0] - size
      if (diameter <= 0 || Math.abs(diameter - tool.diameter) > 0.5)
        return 'The measured ball diameter differs by more than 0.5 mm. Check the gauge width.'
      return result(() => {
        tool.diameter = Number(diameter.toFixed(6))
        return { ball_diameter: tool.diameter }
      })
    }
    const centre: Vec3 = [(left[0] + right[0]) / 2, start[1], start[2]]
    const front = hit(centre, [0, 1]), back = hit(centre, [0, -1])
    if (!front || !back) return noContact
    centre[1] = (front[1] + back[1]) / 2
    const offset = origin(centre, 'XY')
    return { ...offset, finish: () => ({ ...offset.finish(), center_diameter: back[1] - front[1] - tool.diameter }) }
  }
}
