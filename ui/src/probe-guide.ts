import { t } from './i18n'
import type { Angles } from './arc'
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
  | { kind: 'bore' | 'boss' | 'angle' | 'ball' | 'bore_arc' | 'boss_arc' }
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
  { travel: 'y_plus', along: 'X' },
  { travel: 'x_plus', along: 'Y' },
] as const

/** The button text of an edge-angle choice, in the panel's language. */
export function angleChoiceLabel(travel: AngleEdge): string {
  return travel === 'y_plus' ? t('probe.guide.angleFront') : t('probe.guide.angleLeft')
}

export interface GuideInput { label: string; initial: string; min: number; max: number; signed?: boolean }
const inputs = {
  boss: { label: (): string => t('probe.guide.inputDiameter'), initial: '', min: 2, max: 200 },
  ball: { label: (): string => t('probe.guide.inputGauge'), initial: '', min: 2, max: 200 },
  angle: { label: (): string => t('probe.guide.inputDistance'), initial: '50', min: 5, max: 300, signed: true },
  ridge: { label: (): string => t('probe.guide.inputRidge'), initial: '', min: 2, max: 200 },
  boss_arc: { label: (): string => t('probe.guide.inputDiameter'), initial: '', min: 2, max: 200 },
}
/** The three directions a 3-point cycle starts with: around the whole circle. */
export const defaultArcAngles: Angles = [0, 120, 240]

/** The dimension the operator enters for this target, labelled in the panel's language. */
export function guideInput(target: ProbeGuideTarget): GuideInput | undefined {
  if (target.kind !== 'boss' && target.kind !== 'ball' && target.kind !== 'angle' && target.kind !== 'ridge' &&
      target.kind !== 'boss_arc') return undefined
  const { label, ...input } = inputs[target.kind]
  return { ...input, label: label() }
}

const edges = {
  x_plus: { label: 'X−', axis: 0, sign: 1, faceAt: -24 },
  x_minus: { label: 'X+', axis: 0, sign: -1, faceAt: 24 },
  y_plus: { label: 'Y−', axis: 1, sign: 1, faceAt: -18 },
  y_minus: { label: 'Y+', axis: 1, sign: -1, faceAt: 18 },
} as const
const tipRadius = 2

/** The words for one edge of the part, in the panel's language. */
function edgeText(direction: Edge): { face: string; above: string; title: string; setup: string } {
  const edge = edges[direction].label
  switch (direction) {
    case 'x_plus': return { face: t('probe.guide.faceLeft'), above: t('probe.guide.aboveLeft'),
      title: t('probe.guide.titleLeft', { edge }), setup: t('probe.guide.setupLeft') }
    case 'x_minus': return { face: t('probe.guide.faceRight'), above: t('probe.guide.aboveRight'),
      title: t('probe.guide.titleRight', { edge }), setup: t('probe.guide.setupRight') }
    case 'y_plus': return { face: t('probe.guide.faceFront'), above: t('probe.guide.aboveFront'),
      title: t('probe.guide.titleFront', { edge }), setup: t('probe.guide.setupFront') }
    case 'y_minus': return { face: t('probe.guide.faceBack'), above: t('probe.guide.aboveBack'),
      title: t('probe.guide.titleBack', { edge }), setup: t('probe.guide.setupBack') }
  }
}

function position(point: Vec3, axis: 0 | 1 | 2, value: number): Vec3 {
  const next: Vec3 = [...point]
  next[axis] = value
  return next
}

interface Face {
  label: string
  /** The step that traverses above this face. */
  above: string
  start: Vec3
  contact: Vec3
  surface: Vec3
  axis: 0 | 1 | 2
  /** A search along an XY direction rather than an axis: its unit vector. */
  along?: readonly [number, number]
}

/** Representative workpiece geometry. Directions and move order match the native cycles. */
export function probeGuide(target: ProbeGuideTarget, limits: FixedSetterLimits | undefined, angle: AngleEdge, distance: number | null,
  arc: Angles = defaultArcAngles): ProbeGuidePlan {
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
    const { axis, sign } = edges[direction]
    const text = edgeText(direction)
    return {
      label: text.face,
      above: text.above,
      start: position(base, axis, surfaceAt - sign * 13),
      contact: position(base, axis, surfaceAt - sign * tipRadius),
      surface: position(base, axis, surfaceAt),
      axis,
    }
  }

  switch (target.kind) {
    case 'surface':
      if (target.direction === 'z_top') {
        title = t('probe.guide.titleZTop')
        setup = t('probe.guide.setupZTop')
        result = t('probe.guide.resultZTop')
        start = [0, 0, 16]
        datum = [0, 0, 0]
        const top = t('probe.guide.faceTop')
        faces = [{ label: top, above: top, start, contact: [0, 0, tipRadius], surface: datum, axis: 2 }]
      } else {
        const edge = edges[target.direction]
        const text = edgeText(target.direction)
        title = text.title
        setup = text.setup
        result = t('probe.guide.resultEdge', { axis: edge.axis === 0 ? 'X0' : 'Y0' })
        const touch = side(target.direction, [0, 0, -4])
        faces = [touch]
        start = touch.start
        datum = touch.surface
      }
      break
    case 'corner': {
      const x = edges[target.x]
      const y = edges[target.y]
      title = t('probe.guide.titleCorner', { corner: `${x.label} ${y.label}` })
      setup = t('probe.guide.setupCorner', { reach: String(limits?.material_geometry?.reach_mm ?? 10) })
      result = t('probe.guide.resultCorner')
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
      title = corner ? t('probe.guide.titleInsideCorner', { corner: `X${xSign > 0 ? '+' : '−'} Y${ySign > 0 ? '+' : '−'}` })
        : target.kind === 'pocket' ? t('probe.guide.titlePocket')
          : ridge ? t('probe.guide.titleRidge', { axis: axisName }) : t('probe.guide.titleSlot', { axis: axisName })
      setup = ridge ? t('probe.guide.setupRidge')
        : corner ? t('probe.guide.setupInsideCorner')
          : target.kind === 'pocket' ? t('probe.guide.setupPocket') : t('probe.guide.setupSlot')
      result = corner ? t('probe.guide.resultInsideCorner')
        : target.kind === 'pocket' ? t('probe.guide.resultPocket')
          : t('probe.guide.resultWidth', { axis: `${axisName}0` })
      clearance = ridge ? 9 : null
      start = corner ? [xSign * (hx - 5), ySign * (hy - 5), -5] : [2, -2, clearance ?? -5]
      datum = corner ? [hx * xSign, hy * ySign, -3] : [0, 0, 0]
      const wall = (across: 0 | 1, sign: number): Face => {
        const surfaceAt = (across === 0 ? hx : hy) * sign
        const searchAt = ridge ? position([start[0], start[1], -tipRadius - 2], across, surfaceAt + sign * 7) : start
        const contact = position(searchAt, across, surfaceAt + (ridge ? sign : -sign) * tipRadius)
        const name = `${sign > 0 ? '+' : '−'}${across === 0 ? 'X' : 'Y'}`
        return {
          label: ridge ? t('probe.guide.face', { side: name }) : t('probe.guide.wall', { side: name }),
          above: ridge ? t('probe.guide.aboveFace', { side: name }) : t('probe.guide.aboveWall', { side: name }),
          start: searchAt, contact, surface: position(contact, across, surfaceAt), axis: across,
        }
      }
      faces = corner ? [wall(0, xSign), wall(1, ySign)]
        : target.kind === 'pocket' ? [wall(0, -1), wall(0, 1), wall(1, -1), wall(1, 1)]
          : [wall(axis, -1), wall(axis, 1)]
      break
    }
    case 'bore': {
      title = t('probe.guide.titleBore')
      setup = t('probe.guide.setupBore')
      result = t('probe.guide.resultDiameter')
      example = t('probe.guide.exampleCentre', { diameter: '26' })
      object = 'bore'
      start = [2, -3, -4]
      datum = [0, 0, 0]
      const inner = (axis: 0 | 1, sign: number): Face => {
        const base: Vec3 = axis === 0 ? [2, -3, -4] : [0, -3, -4]
        const across = axis === 0 ? 3 : 0
        const centre = Math.sqrt((13 - tipRadius) ** 2 - across ** 2)
        const contact = position(base, axis, sign * centre)
        const name = `${sign > 0 ? '+' : '−'}${axis === 0 ? 'X' : 'Y'}`
        return { label: t('probe.guide.wall', { side: name }), above: t('probe.guide.aboveWall', { side: name }), start: base,
          contact, surface: [contact[0] * 13 / (13 - tipRadius), contact[1] * 13 / (13 - tipRadius), contact[2]], axis }
      }
      faces = [inner(0, 1), inner(0, -1), inner(1, 1), inner(1, -1)]
      break
    }
    case 'bore_arc':
    case 'boss_arc': {
      // Three searches along the chosen directions: outward from inside a bore of radius 13,
      // inward from outside a boss of radius 14.
      const inside = target.kind === 'bore_arc'
      title = inside ? t('probe.guide.titleBoreArc') : t('probe.guide.titleBossArc')
      setup = inside ? t('probe.guide.setupBoreArc') : t('probe.guide.setupBossArc')
      result = t('probe.guide.resultArc')
      example = t('probe.guide.exampleCentre', { diameter: inside ? '26' : '28' })
      object = inside ? 'bore' : 'boss'
      clearance = inside ? null : 9
      start = inside ? [2, -3, -4] : [0, 0, 9]
      datum = [0, 0, 0]
      const z = inside ? -4 : -tipRadius - 2
      faces = arc.map((degrees): Face => {
        const unit: [number, number] = [Math.cos(degrees * Math.PI / 180), Math.sin(degrees * Math.PI / 180)]
        const label = t('probe.guide.contactAt', { angle: String(degrees) })
        const above = t('probe.guide.aboveContact', { angle: String(degrees) })
        if (inside) {
          // From the start, outward to the circle the ball centre touches.
          const [x, y] = [start[0], start[1]]
          const dot = x * unit[0] + y * unit[1]
          const reach = -dot + Math.sqrt(dot * dot - (x * x + y * y - (13 - tipRadius) ** 2))
          const contact: Vec3 = [x + unit[0] * reach, y + unit[1] * reach, z]
          return { label, above, start, contact, along: unit, axis: Math.abs(unit[0]) >= Math.abs(unit[1]) ? 0 : 1,
            surface: [contact[0] + unit[0] * tipRadius, contact[1] + unit[1] * tipRadius, z] }
        }
        const point = (radius: number): Vec3 => [unit[0] * radius, unit[1] * radius, z]
        return { label, above, start: point(27), contact: point(14 + tipRadius), surface: point(14),
          along: [-unit[0], -unit[1]], axis: Math.abs(unit[0]) >= Math.abs(unit[1]) ? 0 : 1 }
      })
      break
    }
    case 'boss':
    case 'ball': {
      const boss = target.kind === 'boss'
      title = boss ? t('probe.guide.titleBoss') : t('probe.guide.titleBall')
      setup = boss ? t('probe.guide.setupBoss') : t('probe.guide.setupBall')
      result = boss ? t('probe.guide.resultDiameter') : t('probe.guide.resultBall')
      example = boss ? t('probe.guide.exampleCentre', { diameter: '28' }) : t('probe.guide.exampleBall', { diameter: '4' })
      object = boss ? 'boss' : 'gauge'
      clearance = 9
      start = [0, 0, clearance]
      datum = [0, 0, 0]
      const depth: Vec3 = [0, 0, -tipRadius - 2]
      faces = [side('x_plus', depth, -14), side('x_minus', depth, 14)]
      if (boss) faces.push(side('y_plus', depth, -14), side('y_minus', depth, 14))
      break
    }
    case 'angle': {
      const edge = edges[angle]
      const along = edge.axis === 0 ? 1 : 0
      const step = (distance ?? 50) < 0 ? -12 : 12
      rotation = 7 * Math.PI / 180
      title = t('probe.guide.titleAngle')
      const alongName = along === 0 ? 'X' : 'Y'
      setup = angle === 'y_plus' ? t('probe.guide.setupAngleFront', { axis: alongName })
        : t('probe.guide.setupAngleLeft', { axis: alongName })
      result = t('probe.guide.resultAngle')
      example = t('probe.guide.exampleAngle', { angle: '+7°' })
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
    case 'check': {
      title = target.kind === 'teach' ? t('probe.guide.titleTeach')
        : target.kind === 'check' ? t('probe.guide.titleCheck') : t('probe.guide.titleMeasure')
      setup = target.kind === 'teach' ? t('probe.guide.setupTeach') : t('probe.guide.setupMeasure')
      result = target.kind === 'teach' ? t('probe.guide.resultTeach')
        : target.kind === 'check' ? t('probe.guide.resultCheck') : t('probe.guide.resultMeasure')
      object = 'setter'
      start = [0, 0, 16]
      datum = [0, 0, 0]
      const setter = t('probe.guide.faceSetter')
      faces = [{ label: setter, above: setter, start, contact: [0, 0, tipRadius], surface: datum, axis: 2 }]
      break
    }
  }

  const steps: ProbeGuideStep[] = [{ label: t('probe.guide.stepStart'), point: start, kind: 'start' }]
  let at = start
  const move = (point: Vec3, label: string): void => {
    if (!at.every((value, axis) => value === point[axis])) steps.push({ point, label, kind: 'move' })
    at = point
  }
  const latch = limits?.latch_feed_mm_min !== undefined ? limits.latch_feed_mm_min > 0
    : limits?.material_travel_mm !== undefined
  for (const face of faces) {
    if (clearance !== null) {
      move(position(at, 2, clearance), t('probe.guide.stepLiftTop'))
      move(position(face.start, 2, clearance), face.above)
      move(face.start, t('probe.guide.stepLower'))
    } else {
      move(face.start, t('probe.guide.stepNext'))
    }
    steps.push({ label: t('probe.guide.stepSearch', { face: face.label }), point: face.contact, kind: 'search', contact: face.surface })
    if (latch) {
      const along = face.along
      const available = along
        ? Math.hypot(face.start[0] - face.contact[0], face.start[1] - face.contact[1])
        : Math.abs(face.start[face.axis] - face.contact[face.axis])
      const requested = limits?.latch_backoff_mm ?? 1
      const backoff = object === 'setter' ? requested : Math.min(available, requested)
      const retreat = face.contact[face.axis] + Math.sign(face.start[face.axis] - face.contact[face.axis]) * backoff
      const back: Vec3 = along
        ? [face.contact[0] - along[0] * backoff, face.contact[1] - along[1] * backoff, face.contact[2]]
        : position(face.contact, face.axis, retreat)
      for (let pass = 1; pass <= 3; pass++) {
        steps.push({ label: t('probe.guide.stepBackOff'), point: back, kind: 'move' },
          { label: t('probe.guide.stepLatch', { pass: String(pass) }), point: face.contact, kind: 'contact', contact: face.surface })
      }
    }
    steps.push({ label: t('probe.guide.stepRetract'), point: face.start, kind: 'move' })
    at = face.start
  }
  if (clearance !== null) move(position(at, 2, clearance), t('probe.guide.stepLiftWork'))
  if (object === 'setter' && limits?.clearance_mm) {
    // Keep the full motion in frame while showing a lift above the search start.
    at = [0, 0, 20]
    steps[steps.length - 1] = { label: t('probe.guide.stepClearance'), point: at, kind: 'move' }
  }
  steps.push({ label: example ?? result, point: at, kind: 'result', contact: datum })
  return { title, setup, result, object, steps, datum, rotation, ...(featureHalf ? { featureHalf } : {}) }
}
