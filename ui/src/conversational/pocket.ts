// Circular and rectangular pockets clear each layer from the centre out. A separate wall pass
// can remove the stock left by roughing before the next layer begins.

import { t } from '../i18n'
import { Checks, Program, checkCommon, feedNumber, number, steps, written, type Common, type Generated } from './program'

export type PocketEntry = 'plunge' | 'helix' | 'ramp'
export type PocketCoolant = 'off' | 'flood' | 'mist' | 'both'

export interface PocketOptions {
  entry?: PocketEntry
  entryAngle?: number
  wallStock?: number
  finish?: boolean
  coolant?: PocketCoolant
}

interface Pocket extends Common, PocketOptions {
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

type Point = readonly [number, number]
type Entry =
  | { kind: 'plunge'; start: Point }
  | { kind: 'helix'; start: Point; radius: number; drop: number }
  | { kind: 'ramp'; start: Point; end: Point; drop: number }

const maxPocketMoves = 12_000

function floorDistance(value: number, c: Pocket): number {
  const scale = c.unit === 'inch' ? 10_000 : 1_000
  return Math.floor(value * scale + 1e-9) / scale
}

function xy(p: Program, [x, y]: Point): string {
  return `X${p.n(x)} Y${p.n(y)}`
}

function checkPocket(c: Pocket, checks: Checks): void {
  checkCommon(c, checks)
  checks.check(Number.isFinite(c.x) && Number.isFinite(c.y), t('operations.check.centre'))
  checks.positive(c.depth, 'operations.label.depth')
  checks.positive(c.depthPerPass, 'operations.label.depthPerPass')
  checks.check(c.stepover > 0 && c.stepover <= 100, t('operations.check.stepover'))
  checks.positive(c.plungeFeed, 'operations.label.entryFeed')
  const stock = c.wallStock ?? 0
  checks.check(Number.isFinite(stock) && stock >= 0, t('operations.check.wallStockNonnegative'))
  if (stock > 0) checks.check(written(stock, c.unit) > 0, t('operations.check.wallStockPrecision'))
  if (c.entry === 'helix' || c.entry === 'ramp') {
    const angle = c.entryAngle ?? 3
    checks.check(angle > 0 && angle < 90, t('operations.check.entryAngle'))
  }
}

/** Entry is contained in the rough footprint, measured at the centre of the tool. */
function entryPath(c: Pocket, halfX: number, halfY: number, checks: Checks): Entry {
  const kind = c.entry ?? 'plunge'
  if (kind === 'plunge') return { kind, start: [c.x, c.y] }
  const x = written(c.x, c.unit)
  const y = written(c.y, c.unit)
  const alongX = halfX >= halfY
  const radius = floorDistance(kind === 'helix' ? Math.min(halfX, halfY, c.tool.diameter / 4)
    : Math.max(halfX, halfY), c)
  const length = kind === 'helix' ? 2 * Math.PI * radius : 2 * radius
  // The allowable drop is rounded down to the G-code's Z resolution before dividing a layer.
  const drop = floorDistance(length * Math.tan((c.entryAngle ?? 3) * Math.PI / 180), c)
  checks.check(radius > 0 && drop > 0, t('operations.check.entryPrecision'))
  if (kind === 'helix') return { kind, start: [x + radius, y], radius, drop }
  return {
    kind,
    start: alongX ? [x - radius, y] : [x, y - radius],
    end: alongX ? [x + radius, y] : [x, y + radius],
    drop,
  }
}

function checkMoveCount(c: Pocket, entry: Entry, passes: number, checks: Checks): void {
  const count = steps(c.depth, c.depthPerPass)
  if (checks.errors.length) return
  const maxDrop = Math.min(c.depth, c.depthPerPass) + (c.unit === 'inch' ? 0.0001 : 0.001)
  const entryMoves = entry.kind === 'plunge' ? 1 : steps(maxDrop, entry.drop) + 3
  checks.check(count * (entryMoves + passes + 1) <= maxPocketMoves, t('operations.check.pocketTooManyMoves'))
}

function descend(p: Program, c: Pocket, entry: Entry, before: number, after: number): void {
  if (entry.kind === 'plunge') {
    p.cut(`G1 Z${p.n(after)}`, c.plungeFeed)
    return
  }
  const count = steps(before - after, entry.drop)
  const depth = (step: number): string => p.n(before + (after - before) * step / count)
  if (entry.kind === 'helix') {
    const arc = `G3 ${xy(p, entry.start)} I${p.n(-entry.radius)} J0`
    for (let step = 1; step <= count; step += 1) p.cut(`${arc} Z${depth(step)}`, c.plungeFeed)
    p.cut(arc, c.plungeFeed)
  } else {
    for (let step = 1; step <= count; step += 1)
      p.cut(`G1 ${xy(p, step % 2 ? entry.end : entry.start)} Z${depth(step)}`, c.plungeFeed)
    p.cut(`G1 ${xy(p, count % 2 ? entry.start : entry.end)}`, c.plungeFeed)
  }
  p.cut(`G1 ${xy(p, [c.x, c.y])}`, c.feed)
}

/** Enter each layer from the previous floor and return to the centre after clearing. */
function layers(p: Program, c: Pocket, entry: Entry, rings: () => void): void {
  const count = steps(c.depth, c.depthPerPass)
  p.add('G91.1')
  if (c.coolant !== undefined) p.add('M9')
  if (c.coolant === 'mist' || c.coolant === 'both') p.add('M7')
  if (c.coolant === 'flood' || c.coolant === 'both') p.add('M8')
  p.add('')
  p.rapidXY(...entry.start)
  if (entry.kind !== 'plunge') p.cut('G1 Z0', c.plungeFeed)
  for (let layer = 1; layer <= count; layer += 1) {
    p.add(`(Layer ${String(layer)} of ${String(count)})`) // i18n-ignore
    if (layer > 1 && entry.kind !== 'plunge') p.cut(`G1 ${xy(p, entry.start)}`, c.feed)
    descend(p, c, entry, written((-c.depth * (layer - 1)) / count, c.unit), written((-c.depth * layer) / count, c.unit))
    rings()
    p.cut(`G1 ${xy(p, [c.x, c.y])}`, c.feed)
  }
  if (c.coolant !== undefined && c.coolant !== 'off') p.add('M9')
}

function depthLine(p: Program, c: Pocket, entry: Entry): string {
  const count = steps(c.depth, c.depthPerPass)
  const method = entry.kind === 'plunge' ? 'plunge straight down at the centre' // i18n-ignore
    : `${entry.kind === 'helix' ? 'helical' : 'linear ramp'} entry, max ${String(c.entryAngle ?? 3)} degrees` // i18n-ignore
  const tool = entry.kind === 'plunge' ? ': centre-cutting end mill' : '' // i18n-ignore
  return `Depth ${p.n(c.depth)} in ${String(count)} ${count === 1 ? 'layer' : 'layers'} of ${p.n(c.depth / count)}, ${method} at ${feedNumber(c.plungeFeed, c.unit)}${tool}` // i18n-ignore
}

function finishStock(p: Program, stock: number, count: number, contour: (inset: number) => void): void {
  if (count === 0) return
  p.add('(Finish walls)') // i18n-ignore
  for (let pass = 1; pass <= count; pass += 1) contour(stock * (1 - pass / count))
}

export function circularPocket(c: CircularPocket): Generated {
  const checks = new Checks()
  checkPocket(c, checks)
  checks.positive(c.diameter, 'operations.label.pocketDiameter')
  const radius = (c.diameter - c.tool.diameter) / 2
  if (c.diameter > 0 && c.tool.diameter > 0)
    checks.check(written(radius, c.unit) > 0, t('operations.check.toolTooBigPocket', { diameter: number(c.tool.diameter, c.unit) }))
  const stock = c.wallStock ?? 0
  const rough = radius - stock
  if (stock > 0 && radius > 0) checks.check(written(rough, c.unit) > 0, t('operations.check.wallStockFits'))
  if (checks.errors.length) return { errors: checks.errors }

  const spacing = (c.tool.diameter * c.stepover) / 100
  const rings = steps(rough, spacing)
  const finish = c.finish && stock > 0 ? steps(stock, spacing) : 0
  const entry = entryPath(c, rough, rough, checks)
  checkMoveCount(c, entry, 2 * (rings + finish), checks)
  if (checks.errors.length) return { errors: checks.errors }

  const p = new Program(c)
  p.start('Circular pocket', [ // i18n-ignore
    `Diameter ${p.n(c.diameter)} at X${p.n(c.x)} Y${p.n(c.y)}`, // i18n-ignore
    `${String(rings)} ${rings === 1 ? 'ring' : 'rings'} ${p.n(rough / rings)} apart, stepover ${String(c.stepover)} %, G3 climb milling`, // i18n-ignore
    ...(stock > 0 ? [`Roughing leaves ${p.n(stock)} wall stock`] : []), // i18n-ignore
    depthLine(p, c, entry),
  ])
  const circle = (r: number): void => {
    if (written(r, c.unit) <= 0) return
    const words = xy(p, [c.x + r, c.y])
    p.cut(`G1 ${words}`, c.feed)
    p.cut(`G3 ${words} I${p.n(-r)} J0`, c.feed)
  }
  layers(p, c, entry, () => {
    for (let ring = 1; ring <= rings; ring += 1) {
      circle(rough * ring / rings)
    }
    finishStock(p, stock, finish, (inset) => { circle(radius - inset) })
  })
  return { text: p.end() }
}

/**
 * One counter-clockwise loop around a rounded rectangle of half-sizes `a` `b` and corner radius
 * `corner` about the centre, from and back to its point on +X. Lines of no length are left out, so
 * a loop with `b` zero is a line there and back. A corner radius written as 0 is a sharp corner.
 */
function loop(p: Program, c: RectangularPocket, a: number, b: number, corner: number): void {
  const r = written(corner, c.unit) > 0 ? corner : 0
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
  checks.positive(c.width, 'operations.label.sizeX')
  checks.positive(c.height, 'operations.label.sizeY')
  const d = c.tool.diameter
  const smaller = Math.min(c.width, c.height)
  if (smaller > 0 && d > 0) checks.check(d < smaller, t('operations.check.toolTooBigPocket', { diameter: number(d, c.unit) }))
  checks.check(c.cornerRadius >= 0 && c.cornerRadius <= smaller / 2, t('operations.check.cornerRadiusRange'))
  if (c.cornerRadius > 0 && d > 0)
    checks.check(c.cornerRadius >= d / 2, t('operations.check.cornerRadiusSmall', { radius: number(d / 2, c.unit) }))
  if (checks.errors.length) return { errors: checks.errors }

  // Half-sizes and corner radius of the tool centre's path at the wall.
  const a = c.width / 2 - d / 2
  const b = c.height / 2 - d / 2
  const r = Math.max(c.cornerRadius - d / 2, 0)
  const stock = c.wallStock ?? 0
  const inner = Math.min(a, b) - stock
  if (stock > 0) checks.check(written(inner, c.unit) > 0, t('operations.check.wallStockFits'))
  if (checks.errors.length) return { errors: checks.errors }
  // Beyond a sharp corner of one loop the next loop out reaches the corner's diagonal only when
  // it is at most (1 + 1/√2)/2 of the tool diameter away; further apart, a pillar stays there.
  const spacing = (d * c.stepover) / 100
  const step = r < Math.min(a, b) ? Math.min(spacing, (d * (1 + Math.SQRT1_2)) / 2) : spacing
  const rings = steps(inner, step)
  const finish = c.finish && stock > 0 ? steps(stock, step) : 0
  const entry = entryPath(c, a - stock, b - stock, checks)
  checkMoveCount(c, entry, 10 * (rings + 1 + finish), checks)
  if (checks.errors.length) return { errors: checks.errors }

  const p = new Program(c)
  p.start('Rectangular pocket', [ // i18n-ignore
    `Size X ${p.n(c.width)} by Y ${p.n(c.height)}, corner radius ${p.n(Math.max(c.cornerRadius, d / 2))}, centre X${p.n(c.x)} Y${p.n(c.y)}`, // i18n-ignore
    `${String(rings + 1)} loops ${p.n(inner / rings)} apart, stepover ${String(c.stepover)} %, G3 climb milling`, // i18n-ignore
    ...(stock > 0 ? [`Roughing leaves ${p.n(stock)} wall stock`] : []), // i18n-ignore
    depthLine(p, c, entry),
  ])
  const contour = (inset: number): void => {
    if (a - inset > 1e-9) p.cut(`G1 X${p.n(c.x + a - inset)} Y${p.n(c.y)}`, c.feed)
    loop(p, c, a - inset, b - inset, Math.max(r - inset, 0))
  }
  layers(p, c, entry, () => {
    // From the line along the longer side out to the wall, each loop the last one offset outward.
    for (let ring = 0; ring <= rings; ring += 1) contour(stock + inner * (rings - ring) / rings)
    finishStock(p, stock, finish, contour)
  })
  return { text: p.end() }
}
