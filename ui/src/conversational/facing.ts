// Facing a rectangle: zig-zag passes across it, at equal depth steps, each pass entering and
// leaving outside the stock.

import { t } from '../i18n'
import { Checks, Program, checkCommon, feedNumber, steps, type Common, type Generated, type OperationsKey } from './program'

export type Origin = 'lower-left' | 'lower-right' | 'upper-left' | 'upper-right' | 'centre'

/** Each origin and the message key of its name. */
export const origins: readonly [Origin, OperationsKey<'option'>][] = [
  ['lower-left', 'operations.option.lowerLeft'], ['lower-right', 'operations.option.lowerRight'],
  ['upper-left', 'operations.option.upperLeft'], ['upper-right', 'operations.option.upperRight'], ['centre', 'operations.option.centre'],
]

export interface Facing extends Common {
  /** Size of the rectangle along X and Y. */
  width: number
  height: number
  /** Where X0 Y0 is on the rectangle. */
  origin: Origin
  /** Passes run along this axis and step over along the other. */
  along: 'x' | 'y'
  /** Percent of the tool diameter. */
  stepover: number
  depth: number
  depthPerPass: number
  plungeFeed: number
}

export function facing(f: Facing): Generated {
  const checks = new Checks()
  checkCommon(f, checks)
  checks.positive(f.width, 'operations.label.sizeX')
  checks.positive(f.height, 'operations.label.sizeY')
  checks.check(f.stepover > 0 && f.stepover <= 100, t('operations.check.stepover'))
  checks.positive(f.depth, 'operations.label.totalDepth')
  checks.positive(f.depthPerPass, 'operations.label.depthPerPass')
  checks.positive(f.plungeFeed, 'operations.label.plungeFeed')
  if (checks.errors.length) return { errors: checks.errors }

  const d = f.tool.diameter
  const xMin = f.origin === 'centre' ? -f.width / 2 : f.origin.endsWith('right') ? -f.width : 0
  const yMin = f.origin === 'centre' ? -f.height / 2 : f.origin.startsWith('upper') ? -f.height : 0
  // a: the axis a pass runs along; b: the axis it steps over along.
  const [aMin, aSize, bMin, bSize] = f.along === 'x' ? [xMin, f.width, yMin, f.height] : [yMin, f.height, xMin, f.width]
  const rows = steps(bSize, (d * f.stepover) / 100)
  const step = bSize / rows
  // The first pass cuts one step of the stock, the last reaches the far edge exactly.
  const row = (k: number): number => bMin - d / 2 + (k + 1) * step
  // Clear of the stock by the tool radius and a tenth of the diameter at both ends.
  const ends = [aMin - 0.6 * d, aMin + aSize + 0.6 * d] as const
  const layers = steps(f.depth, f.depthPerPass)
  const p = new Program(f)
  const [a, b] = f.along === 'x' ? ['X', 'Y'] : ['Y', 'X']
  const corner = f.origin === 'centre' ? 'the centre' : `the ${f.origin} corner` // i18n-ignore
  p.start('Facing', [ // i18n-ignore
    `Rectangle X ${p.n(f.width)} by Y ${p.n(f.height)}, X0 Y0 at ${corner}`, // i18n-ignore
    `Passes along ${a}, zig-zag, ${String(rows)} per layer, stepover ${p.n(step)} = ${String(f.stepover)} %`, // i18n-ignore
    `Depth ${p.n(f.depth)} in ${String(layers)} ${layers === 1 ? 'layer' : 'layers'} of ${p.n(f.depth / layers)}, plunge feed ${feedNumber(f.plungeFeed, f.unit)} outside the stock`, // i18n-ignore
  ])
  for (let layer = 1; layer <= layers; layer += 1) {
    p.add('', `(Layer ${String(layer)} of ${String(layers)})`) // i18n-ignore
    if (f.along === 'x') p.rapidXY(ends[0], row(0))
    else p.rapidXY(row(0), ends[0])
    p.cut(`G1 Z${p.n((-f.depth * layer) / layers)}`, f.plungeFeed)
    for (let k = 0; k < rows; k += 1) {
      p.cut(`G1 ${a}${p.n(ends[(k + 1) % 2 === 1 ? 1 : 0])}`, f.feed)
      if (k + 1 < rows) p.cut(`G1 ${b}${p.n(row(k + 1))}`, f.feed)
    }
    p.rapidZ(f.clearance)
  }
  return { text: p.end() }
}
