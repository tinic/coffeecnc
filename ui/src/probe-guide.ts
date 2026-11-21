import type { FixedSetterLimits, ProbeAxis, ProbeDirection, Vec3 } from './protocol'

type Edge = Exclude<ProbeDirection, 'z_top'>
export type AngleEdge = 'x_plus' | 'y_plus'
export type MaterialGuideTarget =
  | { kind: 'surface'; direction: ProbeDirection }
  | { kind: 'corner'; x: 'x_plus' | 'x_minus'; y: 'y_plus' | 'y_minus' }
  | { kind: 'inside_corner'; x: 'x_plus' | 'x_minus'; y: 'y_plus' | 'y_minus' }
  | { kind: 'pocket' }
  | { kind: 'slot'; axis: ProbeAxis }
  | { kind: 'ridge'; axis: ProbeAxis }
  | { kind: 'bore' | 'boss' | 'angle' | 'ball' }
export type ProbeGuideTarget = MaterialGuideTarget | { kind: 'teach' | 'measure' | 'check' }

export interface ProbeGuideStep {
  label: string
  point: Vec3
  kind: 'start' | 'move' | 'search' | 'contact' | 'result'
  contact?: Vec3
}

export interface ProbeGuidePlan {
  title: string
  setup: string
  result: string
  object: 'stock' | 'bore' | 'boss' | 'gauge' | 'setter' | 'pocket' | 'slot' | 'ridge'
  featureHalf?: [number, number]
  steps: ProbeGuideStep[]
  datum: Vec3
  rotation: number
}

export const angleChoices = [
  { label: 'Front · Y−', name: 'Front', travel: 'y_plus', along: 'X' },
  { label: 'Left · X−', name: 'Left', travel: 'x_plus', along: 'Y' },
] as const

export interface GuideInput { label: string; initial: string; min: number; max: number; signed?: boolean }
const inputs = {
  boss: { label: 'Approximate diameter', initial: '', min: 2, max: 200 },
  ball: { label: 'Gauge width', initial: '', min: 2, max: 200 },
  angle: { label: 'Distance along the edge', initial: '50', min: 5, max: 300, signed: true },
  ridge: { label: 'Approximate ridge width', initial: '', min: 2, max: 200 },
} satisfies Record<string, GuideInput>

export function guideInput(target: ProbeGuideTarget): GuideInput | undefined {
  return target.kind === 'boss' || target.kind === 'ball' || target.kind === 'angle' || target.kind === 'ridge'
    ? inputs[target.kind] : undefined
}

const edges = {
  x_plus: { face: 'Left edge', label: 'X−', axis: 0, sign: 1, faceAt: -24 },
  x_minus: { face: 'Right edge', label: 'X+', axis: 0, sign: -1, faceAt: 24 },
  y_plus: { face: 'Front edge', label: 'Y−', axis: 1, sign: 1, faceAt: -18 },
  y_minus: { face: 'Back edge', label: 'Y+', axis: 1, sign: -1, faceAt: 18 },
} as const
const tipRadius = 2

function position(point: Vec3, axis: 0 | 1 | 2, value: number): Vec3 {
  const next: Vec3 = [...point]
  next[axis] = value
  return next
}

interface Face {
  label: string
  start: Vec3
  contact: Vec3
  surface: Vec3
  axis: 0 | 1 | 2
}

/** Representative workpiece geometry. Directions and move order match the native cycles. */
export function probeGuide(target: ProbeGuideTarget, limits: FixedSetterLimits | undefined, angle: AngleEdge, distance: number | null): ProbeGuidePlan {
  let title: string
  let setup: string
  let result: string
  let example: string | undefined
  let object: ProbeGuidePlan['object'] = 'stock'
  let start: Vec3
  let datum: Vec3
  let faces: Face[] = []
  let clearance: number | null = null
  let rotation = 0
  let featureHalf: [number, number] | undefined
  const side = (direction: Edge, base: Vec3, surfaceAt: number = edges[direction].faceAt): Face => {
    const { face, axis, sign } = edges[direction]
    return {
      label: face,
      start: position(base, axis, surfaceAt - sign * 13),
      contact: position(base, axis, surfaceAt - sign * tipRadius),
      surface: position(base, axis, surfaceAt),
      axis,
    }
  }

  switch (target.kind) {
    case 'surface':
      if (target.direction === 'z_top') {
        title = 'Probe Z top'
        setup = 'Touch off the probe at the setter and apply its length. Jog directly above the top surface.'
        result = 'Set Z0 on the top surface'
        start = [0, 0, 16]
        datum = [0, 0, 0]
        faces = [{ label: 'Top', start, contact: [0, 0, tipRadius], surface: datum, axis: 2 }]
      } else {
        const edge = edges[target.direction]
        title = `Probe ${edge.label} · ${edge.face.toLowerCase()}`
        setup = `Jog beside the ${edge.face.toLowerCase()}, with the ball below the top and aligned with the face.`
        result = `Set ${edge.axis === 0 ? 'X0' : 'Y0'} at the edge`
        const touch = side(target.direction, [0, 0, -4])
        faces = [touch]
        start = touch.start
        datum = touch.surface
      }
      break
    case 'corner': {
      const x = edges[target.x]
      const y = edges[target.y]
      title = `Probe corner ${x.label} ${y.label}`
      setup = `Set Z0 on top first. Jog over the part, with the ball inside and within ${String(limits?.material_geometry?.reach_mm ?? 10)} mm of both selected edges.`
      result = 'Set X0 and Y0 at the corner'
      clearance = 9
      start = [x.faceAt + x.sign * 5, y.faceAt + y.sign * 5, clearance]
      datum = [x.faceAt, y.faceAt, 0]
      faces = [side(target.x, [start[0], start[1], -tipRadius - 2]), side(target.y, [start[0], start[1], -tipRadius - 2])]
      break
    }
    case 'inside_corner':
    case 'pocket':
    case 'slot':
    case 'ridge': {
      const corner = target.kind === 'inside_corner'
      const ridge = target.kind === 'ridge'
      const axis = (target.kind === 'slot' || ridge) && target.axis === 'y' ? 1 : 0
      const axisName = axis === 0 ? 'X' : 'Y'
      const xSign = corner && target.x === 'x_minus' ? -1 : 1
      const ySign = corner && target.y === 'y_minus' ? -1 : 1
      object = corner ? 'pocket' : target.kind
      featureHalf = object === 'pocket' ? [14, 10] : axis === 0 ? [7, 14] : [14, 7]
      const [hx, hy] = featureHalf
      title = corner ? `Probe inside corner X${xSign > 0 ? '+' : '−'} Y${ySign > 0 ? '+' : '−'}`
        : target.kind === 'pocket' ? 'Probe rectangular pocket centre' : `Probe ${target.kind} centre · across ${axisName}`
      setup = ridge
        ? 'Set Z0 on the ridge top first. Jog above the middle and enter the approximate width across the selected axis.'
        : corner
          ? 'Jog the ball inside the recess, below the top and near the selected corner. Both walls must be within search travel.'
          : `Jog the ball inside the ${target.kind}, below the top and roughly centred. Leave enough room for the ball to reach both walls on each measured axis.`
      result = corner ? 'Set X0 and Y0 at the inside corner'
        : target.kind === 'pocket' ? 'Set X0 and Y0 at the centre and report both widths'
          : `Set ${axisName}0 at the centre and report the width`
      clearance = ridge ? 9 : null
      start = corner ? [xSign * (hx - 5), ySign * (hy - 5), -5] : [2, -2, clearance ?? -5]
      datum = corner ? [hx * xSign, hy * ySign, -3] : [0, 0, 0]
      const wall = (across: 0 | 1, sign: number): Face => {
        const surfaceAt = (across === 0 ? hx : hy) * sign
        const searchAt = ridge ? position([start[0], start[1], -tipRadius - 2], across, surfaceAt + sign * 7) : start
        const contact = position(searchAt, across, surfaceAt + (ridge ? sign : -sign) * tipRadius)
        return {
          label: `${sign > 0 ? '+' : '−'}${across === 0 ? 'X' : 'Y'} ${ridge ? 'face' : 'wall'}`,
          start: searchAt, contact, surface: position(contact, across, surfaceAt), axis: across,
        }
      }
      faces = corner ? [wall(0, xSign), wall(1, ySign)]
        : target.kind === 'pocket' ? [wall(0, -1), wall(0, 1), wall(1, -1), wall(1, 1)]
          : [wall(axis, -1), wall(axis, 1)]
      break
    }
    case 'bore': {
      title = 'Probe bore centre'
      setup = 'Jog the ball inside the bore below its top. Leave room to touch both X walls and both Y walls within the search travel.'
      result = 'Set X0 and Y0 at the centre and report the diameter'
      example = 'X0 and Y0 at centre · example Ø26 mm'
      object = 'bore'
      start = [2, -3, -4]
      datum = [0, 0, 0]
      const inner = (axis: 0 | 1, sign: number): Face => {
        const base: Vec3 = axis === 0 ? [2, -3, -4] : [0, -3, -4]
        const across = axis === 0 ? 3 : 0
        const centre = Math.sqrt((13 - tipRadius) ** 2 - across ** 2)
        const contact = position(base, axis, sign * centre)
        return { label: `${sign > 0 ? '+' : '−'}${axis === 0 ? 'X' : 'Y'} wall`, start: base,
          contact, surface: [contact[0] * 13 / (13 - tipRadius), contact[1] * 13 / (13 - tipRadius), contact[2]], axis }
      }
      faces = [inner(0, 1), inner(0, -1), inner(1, 1), inner(1, -1)]
      break
    }
    case 'boss':
    case 'ball': {
      title = target.kind === 'boss' ? 'Probe boss centre' : 'Calibrate the probe ball'
      setup = target.kind === 'boss' ? 'Set Z0 on the boss top first. Jog above the middle and enter its approximate diameter.'
        : 'Set Z0 on the gauge top first. Jog above the middle of a gauge with parallel X faces and enter its known width.'
      result = target.kind === 'boss' ? 'Set X0 and Y0 at the centre and report the diameter' : 'Save the measured ball diameter to the tool table'
      example = target.kind === 'boss' ? 'X0 and Y0 at centre · example Ø28 mm' : 'Save example ball Ø4 mm'
      object = target.kind === 'boss' ? 'boss' : 'gauge'
      clearance = 9
      start = [0, 0, clearance]
      datum = [0, 0, 0]
      const depth: Vec3 = [0, 0, -tipRadius - 2]
      faces = [side('x_plus', depth, -14), side('x_minus', depth, 14)]
      if (target.kind === 'boss') faces.push(side('y_plus', depth, -14), side('y_minus', depth, 14))
      break
    }
    case 'angle': {
      const edge = edges[angle]
      const along = edge.axis === 0 ? 1 : 0
      const step = (distance ?? 50) < 0 ? -12 : 12
      rotation = 7 * Math.PI / 180
      title = 'Measure edge angle'
      setup = `Jog beside the ${edge.face.toLowerCase()} with the ball below the top. Leave room for two touches along ${along === 0 ? 'X' : 'Y'} on the same straight face.`
      result = 'Report the angle. Rotation is applied in a separate confirmation.'
      example = 'Measured angle · example +7°'
      const angledFace = (offset: number): Face => {
        const slope = (edge.axis === 0 ? -1 : 1) * Math.tan(rotation)
        const surface = edge.faceAt / Math.cos(rotation) + offset * slope
        return side(angle, position([0, 0, -4], along, offset), surface)
      }
      const first = angledFace(-step)
      const second = angledFace(step)
      // The second pass begins on the same machine line, shifted only along the edge.
      second.start[edge.axis] = first.start[edge.axis]
      faces = [first, second]
      start = first.start
      datum = second.surface
      break
    }
    case 'teach':
    case 'measure':
    case 'check':
      title = target.kind === 'teach' ? 'Teach the tool setter'
        : target.kind === 'check' ? 'Check tool length' : 'Measure tool length'
      setup = target.kind === 'teach' ? 'Jog the loaded reference tool or a tool with a valid length directly above the centre of the setter.'
        : 'Jog the tool directly above the centre of the taught setter.'
      result = target.kind === 'teach' ? 'Record the setter height and position'
        : target.kind === 'check' ? 'Compare the measured length with the tool table' : 'Save the measured tool length'
      object = 'setter'
      start = [0, 0, 16]
      datum = [0, 0, 0]
      faces = [{ label: 'Setter', start, contact: [0, 0, tipRadius], surface: datum, axis: 2 }]
      break
  }

  const steps: ProbeGuideStep[] = [{ label: 'Jog to the start position', point: start, kind: 'start' }]
  let at = start
  const move = (point: Vec3, label: string): void => {
    if (!at.every((value, axis) => value === point[axis])) steps.push({ point, label, kind: 'move' })
    at = point
  }
  const latch = limits?.latch_feed_mm_min !== undefined ? limits.latch_feed_mm_min > 0
    : limits?.material_travel_mm !== undefined
  for (const face of faces) {
    if (clearance !== null) {
      move(position(at, 2, clearance), 'Lift clear of the top')
      move(position(face.start, 2, clearance), `Move above the ${face.label.toLowerCase()}`)
      move(face.start, 'Lower beside the face')
    } else {
      move(face.start, 'Move to the next search position')
    }
    steps.push({ label: `Search · ${face.label}`, point: face.contact, kind: 'search', contact: face.surface })
    if (latch) {
      const available = Math.abs(face.start[face.axis] - face.contact[face.axis])
      const requested = limits?.latch_backoff_mm ?? 1
      const backoff = object === 'setter' ? requested : Math.min(available, requested)
      const retreat = face.contact[face.axis] + Math.sign(face.start[face.axis] - face.contact[face.axis]) * backoff
      for (let pass = 1; pass <= 3; pass++) {
        steps.push({ label: 'Back off to release', point: position(face.contact, face.axis, retreat), kind: 'move' },
          { label: `Slow latch · ${String(pass)} of 3`, point: face.contact, kind: 'contact', contact: face.surface })
      }
    }
    steps.push({ label: 'Retract', point: face.start, kind: 'move' })
    at = face.start
  }
  if (clearance !== null) move(position(at, 2, clearance), 'Lift clear of the work')
  if (object === 'setter' && limits?.clearance_mm) {
    // Keep the full motion in frame while showing a lift above the search start.
    at = [0, 0, 20]
    steps[steps.length - 1] = { label: 'Retract to clearance', point: at, kind: 'move' }
  }
  steps.push({ label: example ?? result, point: at, kind: 'result', contact: datum })
  return { title, setup, result, object, steps, datum, rotation, ...(featureHalf ? { featureHalf } : {}) }
}
