// Hole patterns: a grid of rows and columns or a bolt circle. G98 returns to the clearance
// height between holes, over any clamp, whether drilling, reaming or boring.

import { t } from '../i18n'
import { Checks, Program, checkCommon, feedNumber, type Common, type Generated } from './program'

export type HoleCycle = 'G81' | 'G83' | 'G85' | 'G89'

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
  cycle: HoleCycle
  /** G83 peck depth. */
  peck: number
  /** G89 dwell at the bottom, in seconds. */
  dwell?: number
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
    checks.count(d.columns, 'operations.label.columns', maxHoles)
    checks.count(d.rows, 'operations.label.rows', maxHoles)
    checks.check(d.columns * d.rows <= maxHoles, t('operations.check.atMostHoles', { max: String(maxHoles) }))
    if (d.columns > 1) checks.check(d.spacingX !== 0 && Number.isFinite(d.spacingX), t('operations.check.columnSpacing'))
    if (d.rows > 1) checks.check(d.spacingY !== 0 && Number.isFinite(d.spacingY), t('operations.check.rowSpacing'))
  } else {
    checks.count(d.count, 'operations.label.numberOfHoles', maxHoles)
    checks.positive(d.radius, 'operations.label.boltCircleRadius')
    checks.check(Number.isFinite(d.startAngle), t('operations.check.startAngle'))
  }
  checks.check(Number.isFinite(d.x) && Number.isFinite(d.y), t('operations.check.position'))
  checks.positive(d.depth, 'operations.label.holeDepth')
  checks.positive(d.retract, 'operations.label.retractHeight')
  checks.check(!(d.retract > d.clearance), t('operations.check.retractAboveClearance'))
  if (d.cycle === 'G83') checks.positive(d.peck, 'operations.label.peckDepth')
  const dwell = Number((d.dwell ?? NaN).toFixed(3))
  if (d.cycle === 'G89') checks.positive(dwell, 'operations.label.boreDwell')
  if (checks.errors.length) return { errors: checks.errors }

  const p = new Program(d)
  const points = holes(d)
  const where = d.pattern === 'grid'
    ? `Grid of ${String(d.columns)} columns by ${String(d.rows)} rows from X${p.n(d.x)} Y${p.n(d.y)}, spacing X ${p.n(d.spacingX)} Y ${p.n(d.spacingY)}` // i18n-ignore
    : `Bolt circle of ${String(d.count)} holes, centre X${p.n(d.x)} Y${p.n(d.y)}, radius ${p.n(d.radius)}, first hole at ${String(d.startAngle)} degrees` // i18n-ignore
  const title = {
    G81: 'Hole pattern', G83: 'Hole pattern', G85: 'Reaming hole pattern', G89: 'Boring hole pattern', // i18n-ignore
  }[d.cycle]
  const cycle = {
    G81: 'G81 drilling', G83: `G83 peck drilling, pecks of ${p.n(d.peck)}`, // i18n-ignore
    G85: 'G85 reaming, feed out to R', G89: `G89 boring, dwell ${String(dwell)} s, feed out to clearance`, // i18n-ignore
  }[d.cycle]
  p.start(title, [
    where,
    `${cycle} to depth ${p.n(d.depth)}, R plane Z${p.n(d.retract)}`, // i18n-ignore
    'G98: back to the clearance height between holes', // i18n-ignore
  ])
  const [first, ...rest] = points
  if (first === undefined) return { errors: [t('operations.check.noHole')] }
  p.add('')
  p.rapidXY(first[0], first[1])
  const cycleWord = d.cycle === 'G83' ? ` Q${p.n(d.peck)}` : d.cycle === 'G89' ? ` P${String(dwell)}` : ''
  p.add(`G98 ${d.cycle} X${p.n(first[0])} Y${p.n(first[1])} Z${p.n(-d.depth)} R${p.n(d.retract)}${cycleWord} F${feedNumber(d.feed, d.unit)}`)
  for (const [x, y] of rest) p.add(`X${p.n(x)} Y${p.n(y)}`)
  p.add('G80')
  return { text: p.end() }
}
