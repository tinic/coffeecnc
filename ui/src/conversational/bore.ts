// Helical bore: the tool circles down at the bore's wall (G3, climb milling inside it), one full
// turn per pitch, then once around the flat bottom. A finish pass leaves `finishAllowance` on
// the wall on the way down and cuts it in a helix back up.

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
  checks.check(Number.isFinite(b.x) && Number.isFinite(b.y), 'Enter the centre X and Y.')
  checks.positive(b.diameter, 'Bore diameter')
  checks.positive(b.depth, 'Depth')
  checks.positive(b.pitch, 'Helix pitch')
  checks.positive(b.plungeFeed, 'Plunge feed')
  const tool = b.tool.diameter
  const radius = (b.diameter - tool) / 2
  if (b.diameter > 0 && tool > 0) {
    checks.check(written(radius, b.unit) > 0, `The tool (diameter ${number(tool, b.unit)}) is too big for this bore.`)
    checks.check(b.diameter <= 2 * tool, 'The bore is more than twice the tool diameter: a core would stay in the middle. Use a circular pocket.')
  }
  if (b.finish) {
    checks.positive(b.finishAllowance, 'Finish allowance')
    checks.check(!(written(radius, b.unit) > 0) || written(radius - b.finishAllowance, b.unit) > 0, 'The finish allowance must be less than half the difference of bore and tool diameter.')
  }
  if (checks.errors.length) return { errors: checks.errors }

  const p = new Program(b)
  const turns = steps(b.depth, b.pitch)
  const rough = b.finish ? radius - b.finishAllowance : radius
  p.start('Helical bore', [
    `Bore diameter ${p.n(b.diameter)} at X${p.n(b.x)} Y${p.n(b.y)}, depth ${p.n(b.depth)}`,
    `Helix of ${String(turns)} ${turns === 1 ? 'turn' : 'turns'}, ${p.n(b.depth / turns)} per turn, G3 climb milling, plunge feed ${feedNumber(b.plungeFeed, b.unit)}`,
    ...(b.finish ? [`Finish pass: ${p.n(b.finishAllowance)} left on the wall, cut in a helix back up`] : []),
  ])
  const circle = (r: number, z?: number): void => {
    p.cut(`G3 X${p.n(b.x + r)} Y${p.n(b.y)}${z === undefined ? '' : ` Z${p.n(z)}`} I${p.n(-r)} J0`, b.feed)
  }
  p.add('')
  p.rapidXY(b.x + rough, b.y)
  p.cut('G1 Z0', b.plungeFeed)
  for (let turn = 1; turn <= turns; turn += 1) circle(rough, (-b.depth * turn) / turns)
  p.add('(Flat bottom)')
  circle(rough)
  if (b.finish) {
    p.add('(Finish pass)')
    p.cut(`G1 X${p.n(b.x + radius)}`, b.feed)
    circle(radius)
    for (let turn = turns - 1; turn >= 0; turn -= 1) circle(radius, (-b.depth * turn) / turns)
  }
  p.cut(`G1 X${p.n(b.x)} Y${p.n(b.y)}`, b.feed)
  return { text: p.end() }
}
