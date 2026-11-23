// Helical bore: the tool circles down at the bore's wall (G3, climb milling inside it) at the plunge
// feed, one full turn per pitch, then once around the flat bottom at the feed. A finish pass leaves `finishAllowance` on
// the wall on the way down and cuts it in a helix back up.

import { t } from '../i18n'
import { Checks, Program, checkCommon, feedNumber, number, steps, written, type Common, type Generated } from './program'

export interface Bore extends Common {
  x: number
  y: number
  diameter: number
  depth: number
  /** Depth per turn of the helix. */
  pitch: number
  finish: boolean
  /** Radial stock left for the finish pass. */
  finishAllowance: number
  plungeFeed: number
}

export function bore(b: Bore): Generated {
  const checks = new Checks()
  checkCommon(b, checks)
  checks.check(Number.isFinite(b.x) && Number.isFinite(b.y), t('operations.check.centre'))
  checks.positive(b.diameter, 'operations.label.boreDiameter')
  checks.positive(b.depth, 'operations.label.depth')
  checks.positive(b.pitch, 'operations.label.helixPitch')
  checks.positive(b.plungeFeed, 'operations.label.plungeFeed')
  const tool = b.tool.diameter
  const radius = (b.diameter - tool) / 2
  if (b.diameter > 0 && tool > 0) {
    checks.check(written(radius, b.unit) > 0, t('operations.check.toolTooBigBore', { diameter: number(tool, b.unit) }))
    checks.check(b.diameter <= 2 * tool, t('operations.check.boreTooBig'))
  }
  if (b.finish) {
    checks.positive(b.finishAllowance, 'operations.label.finishAllowance')
    checks.check(!(written(radius, b.unit) > 0) || written(radius - b.finishAllowance, b.unit) > 0, t('operations.check.finishAllowance'))
  }
  if (checks.errors.length) return { errors: checks.errors }

  const p = new Program(b)
  const turns = steps(b.depth, b.pitch)
  const rough = b.finish ? radius - b.finishAllowance : radius
  p.start('Helical bore', [ // i18n-ignore
    `Bore diameter ${p.n(b.diameter)} at X${p.n(b.x)} Y${p.n(b.y)}, depth ${p.n(b.depth)}`, // i18n-ignore
    `Helix of ${String(turns)} ${turns === 1 ? 'turn' : 'turns'}, ${p.n(b.depth / turns)} per turn, G3 climb milling, plunge feed ${feedNumber(b.plungeFeed, b.unit)}`, // i18n-ignore
    ...(b.finish ? [`Finish pass: ${p.n(b.finishAllowance)} left on the wall, cut in a helix back up`] : []), // i18n-ignore
  ])
  const circle = (r: number, z?: number, feed = b.feed): void => {
    p.cut(`G3 X${p.n(b.x + r)} Y${p.n(b.y)}${z === undefined ? '' : ` Z${p.n(z)}`} I${p.n(-r)} J0`, feed)
  }
  p.add('')
  p.rapidXY(b.x + rough, b.y)
  p.cut('G1 Z0', b.plungeFeed)
  for (let turn = 1; turn <= turns; turn += 1) circle(rough, (-b.depth * turn) / turns, b.plungeFeed)
  p.add('(Flat bottom)') // i18n-ignore
  circle(rough)
  if (b.finish) {
    p.add('(Finish pass)') // i18n-ignore
    p.cut(`G1 X${p.n(b.x + radius)}`, b.feed)
    circle(radius)
    for (let turn = turns - 1; turn >= 0; turn -= 1) circle(radius, (-b.depth * turn) / turns)
  }
  p.cut(`G1 X${p.n(b.x)} Y${p.n(b.y)}`, b.feed)
  return { text: p.end() }
}
