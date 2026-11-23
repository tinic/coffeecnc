import { t } from '../i18n'
import { Checks, Program, checkCommon, feedNumber, steps, written, type Common, type Generated } from './program'

type Point = [number, number]
interface Segment {
  end: Point
  centre?: Point
}

export type Contour = Common & {
  x: number
  y: number
  inside: boolean
  clockwise: boolean
  depth: number
  depthPerPass: number
  plungeFeed: number
} & ({ shape: 'circle'; diameter: number } | { shape: 'rectangle'; width: number; height: number; corner: number })

/** Counterclockwise from the midpoint of the bottom edge, with the origin at the centre. */
function rectangle(x: number, y: number, r: number): Segment[] {
  if (r === 0) return [
    { end: [x, -y] }, { end: [x, y] }, { end: [-x, y] }, { end: [-x, -y] }, { end: [0, -y] },
  ]
  return [
    { end: [x - r, -y] }, { end: [x, -y + r], centre: [x - r, -y + r] },
    { end: [x, y - r] }, { end: [x - r, y], centre: [x - r, y - r] },
    { end: [-x + r, y] }, { end: [-x, y - r], centre: [-x + r, y - r] },
    { end: [-x, -y + r] }, { end: [-x + r, -y], centre: [-x + r, -y + r] },
    { end: [0, -y] },
  ]
}

/** Distance from a point to the border of a rounded rectangle, negative on the inside. */
function distanceToRectangle(point: Point, x: number, y: number, r: number): number {
  const dx = Math.abs(point[0]) - (x - r)
  const dy = Math.abs(point[1]) - (y - r)
  return Math.hypot(Math.max(dx, 0), Math.max(dy, 0)) + Math.min(Math.max(dx, dy), 0) - r
}

export function contour(c: Contour): Generated {
  const checks = new Checks()
  checkCommon(c, checks)
  checks.check(Number.isFinite(c.x) && Number.isFinite(c.y), t('operations.check.centre'))
  checks.positive(c.depth, 'operations.label.depth')
  checks.positive(c.depthPerPass, 'operations.label.depthPerPass')
  checks.positive(c.plungeFeed, 'operations.label.plungeFeed')
  if (c.shape === 'circle') checks.positive(c.diameter, 'operations.label.profileDiameter')
  else {
    checks.positive(c.width, 'operations.label.sizeX')
    checks.positive(c.height, 'operations.label.sizeY')
  }
  if (checks.errors.length) return { errors: checks.errors }

  const rounded = (value: number): number => written(value, c.unit)
  const layers = steps(c.depth, c.depthPerPass)
  checks.check(layers <= 2000 && rounded(c.depth / layers) > 0 && rounded(c.clearance) > 0
    && Number(feedNumber(c.feed, c.unit)) > 0 && Number(feedNumber(c.plungeFeed, c.unit)) > 0,
  t('operations.check.profilePrecision'))
  const radius = c.tool.diameter / 2
  const halfX = rounded((c.shape === 'circle' ? c.diameter : c.width) / 2)
  const halfY = c.shape === 'circle' ? halfX : rounded(c.height / 2)
  const corner = c.shape === 'circle' ? 0 : rounded(c.corner)
  checks.check(halfX > 0 && halfY > 0, t('operations.check.profilePrecision'))
  if (c.shape === 'rectangle') {
    checks.check(c.corner >= 0 && corner < Math.min(halfX, halfY), t('operations.check.profileCorner'))
    if (c.inside) checks.check(corner > radius, t('operations.check.profileInsideCorner', { radius: rounded(radius) }))
  }

  const grid = c.unit === 'inch' ? 0.0001 : 0.001
  const ceil = (value: number): number => rounded(Math.ceil(value / grid) * grid)
  // Roll into the profile in the waste. Give G41/G42 a straight lead longer than the
  // radius and leave room to unwind G40 above the stock on a move longer than the diameter.
  const roll = ceil(1.6 * radius + grid)
  const lead = ceil(1.1 * radius + grid)
  const side = c.inside ? 1 : -1
  const direction = c.clockwise ? -1 : 1
  const start: Point = [0, -halfY]
  const centre: Point = [0, start[1] + side * roll]
  const entry: Point = [-direction * roll, centre[1]]
  const exit: Point = [direction * roll, centre[1]]
  const approach: Point = [entry[0], entry[1] + side * lead]
  if (c.inside) {
    const room = c.shape === 'circle'
      ? Math.hypot(...approach) - halfX
      : distanceToRectangle(approach, halfX, halfY, corner)
    checks.check(room < -radius && (c.shape === 'circle' || roll < halfX - corner),
      t('operations.check.profileLeadRoom'))
  }
  if (checks.errors.length) return { errors: [...new Set(checks.errors)] }

  const p = new Program(c)
  const x = rounded(c.x)
  const y = rounded(c.y)
  const xy = (point: Point): string => `X${p.n(x + point[0])} Y${p.n(y + point[1])}`
  const cutArc = (code: string, from: Point, to: Point, at: Point): void => {
    p.cut(`${code} ${xy(to)} I${p.n(at[0] - from[0])} J${p.n(at[1] - from[1])}`, c.feed)
  }
  const rollCode = side * direction > 0 ? 'G3' : 'G2'
  const profileCode = c.clockwise ? 'G2' : 'G3'
  const comp = side * direction > 0 ? 'G41' : 'G42'
  const mirror = ([a, b]: Point): Point => [direction * a, b]
  const path: Segment[] = c.shape === 'circle' ? [{ end: start, centre: [0, 0] }] : rectangle(halfX, halfY, corner)
  p.start('Contour', [ // i18n-ignore
    `${c.inside ? 'Inside' : 'Outside'} ${c.shape}, ${c.clockwise ? 'clockwise' : 'counterclockwise'}`, // i18n-ignore
    `Centre ${xy([0, 0])}, depth ${p.n(c.depth)} in ${String(layers)} passes`, // i18n-ignore
    `Clear the lead at ${xy(approach)} before plunging. Cutter diameter comes from D${String(c.tool.number)}.`, // i18n-ignore
  ])
  p.add('G91.1')
  for (let layer = 1; layer <= layers; layer += 1) {
    p.rapidXY(x + approach[0], y + approach[1])
    p.cut(`G1 Z${p.n((-c.depth * layer) / layers)}`, c.plungeFeed)
    p.cut(`${comp} D${String(c.tool.number)} G1 ${xy(entry)}`, c.feed)
    cutArc(rollCode, entry, start, centre)
    let from = start
    for (const segment of path) {
      const to = mirror(segment.end)
      if (segment.centre) cutArc(profileCode, from, to, mirror(segment.centre))
      else p.cut(`G1 ${xy(to)}`, c.feed)
      from = to
    }
    cutArc(rollCode, start, exit, centre)
    p.rapidZ(c.clearance)
    p.add(`G40 G0 ${xy(approach)}`)
  }
  return { text: p.end() }
}
