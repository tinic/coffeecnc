// Circular and rectangular pockets: a plunge at the centre for each depth step, then rings from
// the centre out to the wall, at most one stepover apart, counter-clockwise (G3, climb milling
// inside the pocket). Plunging needs a centre-cutting end mill.

import { Checks, Program, checkCommon, feedNumber, number, steps, type Common, type Generated } from './program'

interface Pocket extends Common {
  x: number
  y: number
  depth: number
  depthPerPass: number
  /** Percent of the tool diameter. */
  stepover: number
  plungeFeed: number
}

export interface CircularPocket extends Pocket {
  diameter: number
}

export interface RectangularPocket extends Pocket {
  width: number
  height: number
  /** Radius of the pocket's corners; zero leaves the tool's own radius there. */
  cornerRadius: number
}

function checkPocket(c: Pocket, checks: Checks): void {
  checkCommon(c, checks)
  checks.check(Number.isFinite(c.x) && Number.isFinite(c.y), 'Enter the centre X and Y.')
  checks.positive(c.depth, 'Depth')
  checks.positive(c.depthPerPass, 'Depth per pass')
  checks.check(c.stepover > 0 && c.stepover <= 100, 'Stepover must be more than 0 % and at most 100 % of the tool diameter.')
  checks.positive(c.plungeFeed, 'Plunge feed')
}

/** Every depth step: down at the centre, the rings, back to the centre. */
function layers(p: Program, c: Pocket, rings: (p: Program) => void): void {
  const count = steps(c.depth, c.depthPerPass)
  p.add('')
  p.rapidXY(c.x, c.y)
  for (let layer = 1; layer <= count; layer += 1) {
    p.add(`(Layer ${String(layer)} of ${String(count)})`)
    p.cut(`G1 Z${p.n((-c.depth * layer) / count)}`, c.plungeFeed)
    rings(p)
    p.cut(`G1 X${p.n(c.x)} Y${p.n(c.y)}`, c.feed)
  }
}

function depthLine(p: Program, c: Pocket): string {
  const count = steps(c.depth, c.depthPerPass)
  return `Depth ${p.n(c.depth)} in ${String(count)} ${count === 1 ? 'layer' : 'layers'} of ${p.n(c.depth / count)}, plunge at the centre at ${feedNumber(c.plungeFeed, c.unit)}`
}

export function circularPocket(c: CircularPocket): Generated {
  const checks = new Checks()
  checkPocket(c, checks)
  checks.positive(c.diameter, 'Pocket diameter')
  const radius = (c.diameter - c.tool.diameter) / 2
  if (c.diameter > 0 && c.tool.diameter > 0)
    checks.check(radius > 0, `The tool (diameter ${number(c.tool.diameter, c.unit)}) is too big for this pocket.`)
  if (checks.errors.length) return { errors: checks.errors }

  const p = new Program(c)
  const rings = steps(radius, (c.tool.diameter * c.stepover) / 100)
  p.start('Circular pocket', [
    `Diameter ${p.n(c.diameter)} at X${p.n(c.x)} Y${p.n(c.y)}`,
    `${String(rings)} ${rings === 1 ? 'ring' : 'rings'} ${p.n(radius / rings)} apart, stepover ${String(c.stepover)} %, G3 climb milling`,
    depthLine(p, c),
  ])
  layers(p, c, () => {
    for (let ring = 1; ring <= rings; ring += 1) {
      const r = (radius * ring) / rings
      p.cut(`G1 X${p.n(c.x + r)} Y${p.n(c.y)}`, c.feed)
      p.cut(`G3 X${p.n(c.x + r)} Y${p.n(c.y)} I${p.n(-r)} J0`, c.feed)
    }
  })
  return { text: p.end() }
}

/**
 * One counter-clockwise loop around a rounded rectangle of half-sizes `a` `b` and corner radius
 * `r` about the centre, from and back to its point on +X. Lines of no length are left out, so a
 * loop with `b` zero is a line there and back.
 */
function loop(p: Program, c: RectangularPocket, a: number, b: number, r: number): void {
  let at: [number, number] = [a, 0]
  const same = (u: number, v: number): boolean => Math.abs(u - v) < 1e-9
  const line = (x: number, y: number): void => {
    if (same(x, at[0]) && same(y, at[1])) return
    p.cut(`G1 X${p.n(c.x + x)} Y${p.n(c.y + y)}`, c.feed)
    at = [x, y]
  }
  const arc = (x: number, y: number, i: number, j: number): void => {
    if (r <= 0) return
    p.cut(`G3 X${p.n(c.x + x)} Y${p.n(c.y + y)} I${p.n(i)} J${p.n(j)}`, c.feed)
    at = [x, y]
  }
  line(a, b - r)
  arc(a - r, b, -r, 0)
  line(-a + r, b)
  arc(-a, b - r, 0, -r)
  line(-a, -b + r)
  arc(-a + r, -b, r, 0)
  line(a - r, -b)
  arc(a, -b + r, 0, r)
  line(a, 0)
}

export function rectangularPocket(c: RectangularPocket): Generated {
  const checks = new Checks()
  checkPocket(c, checks)
  checks.positive(c.width, 'Size X')
  checks.positive(c.height, 'Size Y')
  const d = c.tool.diameter
  const smaller = Math.min(c.width, c.height)
  if (smaller > 0 && d > 0) checks.check(d < smaller, `The tool (diameter ${number(d, c.unit)}) is too big for this pocket.`)
  checks.check(c.cornerRadius >= 0 && c.cornerRadius <= smaller / 2, 'The corner radius must be from zero to half the smaller side.')
  if (c.cornerRadius > 0 && d > 0)
    checks.check(c.cornerRadius >= d / 2, `The corner radius is smaller than the tool radius (${number(d / 2, c.unit)}). Enter 0 or at least the tool radius.`)
  if (checks.errors.length) return { errors: checks.errors }

  // Half-sizes and corner radius of the tool centre's path at the wall.
  const a = c.width / 2 - d / 2
  const b = c.height / 2 - d / 2
  const r = Math.max(c.cornerRadius - d / 2, 0)
  const inner = Math.min(a, b)
  const rings = steps(inner, (d * c.stepover) / 100)
  const p = new Program(c)
  p.start('Rectangular pocket', [
    `Size X ${p.n(c.width)} by Y ${p.n(c.height)}, corner radius ${p.n(Math.max(c.cornerRadius, d / 2))}, centre X${p.n(c.x)} Y${p.n(c.y)}`,
    `${String(rings + 1)} loops ${p.n(inner / rings)} apart, stepover ${String(c.stepover)} %, G3 climb milling`,
    depthLine(p, c),
  ])
  layers(p, c, () => {
    // From the line along the longer side out to the wall, each loop the last one offset outward.
    for (let ring = 0; ring <= rings; ring += 1) {
      const offset = (inner * (rings - ring)) / rings
      if (a - offset > 1e-9) p.cut(`G1 X${p.n(c.x + a - offset)} Y${p.n(c.y)}`, c.feed)
      loop(p, c, a - offset, b - offset, Math.max(r - offset, 0))
    }
  })
  return { text: p.end() }
}
