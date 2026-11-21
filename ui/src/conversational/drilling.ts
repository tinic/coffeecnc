// Hole patterns: a grid of rows and columns, or a bolt circle, drilled with G81 or with G83
// pecks. G98 returns to the clearance height between holes, over any clamp.

import { Checks, Program, checkCommon, feedNumber, type Common, type Generated } from './program'

export interface Drilling extends Common {
  pattern: 'grid' | 'circle'
  /** Grid: the first hole, and the spacing of the columns (X) and rows (Y). */
  x: number
  y: number
  columns: number
  rows: number
  spacingX: number
  spacingY: number
  /** Bolt circle: its centre is `x` `y`. Holes counter-clockwise from the start angle. */
  count: number
  radius: number
  /** Degrees from +X. */
  startAngle: number
  cycle: 'G81' | 'G83'
  /** G83 peck depth. */
  peck: number
  depth: number
  /** The R plane, above Z0, where the feed starts. */
  retract: number
}

const maxHoles = 1000

/** The holes in drilling order: grid rows back and forth, the circle counter-clockwise. */
export function holes(d: Drilling): [number, number][] {
  const points: [number, number][] = []
  if (d.pattern === 'circle') {
    for (let i = 0; i < d.count; i += 1) {
      const angle = ((d.startAngle + (360 * i) / d.count) * Math.PI) / 180
      points.push([d.x + d.radius * Math.cos(angle), d.y + d.radius * Math.sin(angle)])
    }
    return points
  }
  for (let row = 0; row < d.rows; row += 1) {
    for (let k = 0; k < d.columns; k += 1) {
      const column = row % 2 === 0 ? k : d.columns - 1 - k
      points.push([d.x + column * d.spacingX, d.y + row * d.spacingY])
    }
  }
  return points
}

export function drilling(d: Drilling): Generated {
  const checks = new Checks()
  checkCommon(d, checks)
  if (d.pattern === 'grid') {
    checks.count(d.columns, 'Columns', maxHoles)
    checks.count(d.rows, 'Rows', maxHoles)
    checks.check(d.columns * d.rows <= maxHoles, `At most ${String(maxHoles)} holes.`)
    if (d.columns > 1) checks.check(d.spacingX !== 0 && Number.isFinite(d.spacingX), 'Column spacing must not be zero.')
    if (d.rows > 1) checks.check(d.spacingY !== 0 && Number.isFinite(d.spacingY), 'Row spacing must not be zero.')
  } else {
    checks.count(d.count, 'Number of holes', maxHoles)
    checks.positive(d.radius, 'Bolt circle radius')
    checks.check(Number.isFinite(d.startAngle), 'Enter a start angle.')
  }
  checks.check(Number.isFinite(d.x) && Number.isFinite(d.y), 'Enter X and Y.')
  checks.positive(d.depth, 'Hole depth')
  checks.positive(d.retract, 'Retract height')
  checks.check(!(d.retract > d.clearance), 'The retract height must not be above the clearance height.')
  if (d.cycle === 'G83') checks.positive(d.peck, 'Peck depth')
  if (checks.errors.length) return { errors: checks.errors }

  const p = new Program(d)
  const points = holes(d)
  const where = d.pattern === 'grid'
    ? `Grid of ${String(d.columns)} columns by ${String(d.rows)} rows from X${p.n(d.x)} Y${p.n(d.y)}, spacing X ${p.n(d.spacingX)} Y ${p.n(d.spacingY)}`
    : `Bolt circle of ${String(d.count)} holes, centre X${p.n(d.x)} Y${p.n(d.y)}, radius ${p.n(d.radius)}, first hole at ${String(d.startAngle)} degrees`
  p.start('Hole pattern', [
    where,
    `${d.cycle === 'G83' ? `G83 peck drilling, pecks of ${p.n(d.peck)}` : 'G81 drilling'} to depth ${p.n(d.depth)}, R plane Z${p.n(d.retract)}`,
    'G98: back to the clearance height between holes',
  ])
  const [first, ...rest] = points
  if (first === undefined) return { errors: ['There is no hole to drill.'] }
  p.add('')
  p.rapidXY(first[0], first[1])
  const q = d.cycle === 'G83' ? ` Q${p.n(d.peck)}` : ''
  p.add(`G98 ${d.cycle} X${p.n(first[0])} Y${p.n(first[1])} Z${p.n(-d.depth)} R${p.n(d.retract)}${q} F${feedNumber(d.feed, d.unit)}`)
  for (const [x, y] of rest) p.add(`X${p.n(x)} Y${p.n(y)}`)
  p.add('G80')
  return { text: p.end() }
}
