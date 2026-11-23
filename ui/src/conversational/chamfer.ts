import { t } from '../i18n'
import { rectangle, type Point, type Segment } from './contour'
import { Checks, Program, checkCommon, feedNumber, unitName, written, type Common, type Generated } from './program'

export type Chamfer = Common & {
  x: number
  y: number
  inside: boolean
  /** Width on the top face, perpendicular to the original edge. */
  chamferWidth: number
  includedAngle: number
  tipDiameter: number
  /** How far the tool tip projects below the bottom edge of the chamfer. */
  tipBelow: number
  /** Clear depth of an already-cut hole or pocket. */
  openingDepth: number
  entryFeed: number
} & ({ shape: 'circle'; diameter: number } | { shape: 'rectangle'; width: number; height: number; corner: number })

export function chamfer(c: Chamfer): Generated {
  const checks = new Checks()
  checkCommon(c, checks)
  checks.check(Number.isFinite(c.x) && Number.isFinite(c.y), t('operations.check.centre'))
  checks.positive(c.chamferWidth, 'operations.label.chamferWidth')
  checks.positive(c.tipBelow, 'operations.label.chamferTipBelow')
  checks.positive(c.entryFeed, 'operations.label.entryFeed')
  checks.check(c.includedAngle > 0 && c.includedAngle < 180, t('operations.check.chamferAngle'))
  checks.check(c.tipDiameter >= 0 && c.tipDiameter < c.tool.diameter, t('operations.check.chamferTip'))
  if (c.inside) checks.positive(c.openingDepth, 'operations.label.chamferOpeningDepth')
  if (c.shape === 'circle') checks.positive(c.diameter, 'operations.label.profileDiameter')
  else {
    checks.positive(c.width, 'operations.label.sizeX')
    checks.positive(c.height, 'operations.label.sizeY')
  }
  if (checks.errors.length) return { errors: checks.errors }

  const grid = c.unit === 'inch' ? 0.0001 : 0.001
  const round = (value: number): number => written(value, c.unit)
  const ceil = (value: number): number => round(Math.ceil(value / grid - 1e-9) * grid)
  const floor = (value: number): number => round(Math.floor(value / grid + 1e-9) * grid)
  const slope = Math.tan(c.includedAngle * Math.PI / 360)
  // The bottom radius sets the XY offset. The larger radius at Z0 sets the cutter fit.
  // Round the offset toward the waste and the cut depth toward the top face.
  const offset = ceil(c.tipDiameter / 2 + c.tipBelow * slope)
  const tipDepth = floor(c.tipBelow + c.chamferWidth / slope)
  const topRadius = c.tipDiameter / 2 + tipDepth * slope
  const halfX = round((c.shape === 'circle' ? c.diameter : c.width) / 2)
  const halfY = c.shape === 'circle' ? halfX : round(c.height / 2)
  const corner = c.shape === 'circle' ? 0 : round(c.corner)
  const side = c.inside ? 1 : -1
  const pathX = round(halfX - side * offset)
  const pathY = round(halfY - side * offset)
  const pathCorner = round(corner - side * offset)
  checks.check(halfX > 0 && halfY > 0 && offset >= grid && tipDepth > 0 && round(topRadius - offset) > 0
    && round(c.clearance) > 0 && Number(feedNumber(c.feed, c.unit)) > 0
    && Number(feedNumber(c.entryFeed, c.unit)) > 0 && Math.round(c.rpm) > 0,
  t('operations.check.chamferPrecision'))
  checks.check(topRadius <= c.tool.diameter / 2 + 1e-9, t('operations.check.chamferCutter'))
  if (!c.inside) checks.check(c.chamferWidth < Math.min(halfX, halfY), t('operations.check.chamferTooWide'))
  if (c.shape === 'rectangle') {
    checks.check(c.corner >= 0 && corner < Math.min(halfX, halfY), t('operations.check.profileCorner'))
    if (c.inside) checks.check(pathCorner >= grid,
      t('operations.check.profileInsideCorner', { radius: offset }))
  }
  if (c.inside) {
    checks.check(floor(c.openingDepth) > tipDepth, t('operations.check.chamferOpeningDepth', { depth: `${String(tipDepth)} ${unitName(c.unit)}` }))
    checks.check(topRadius < Math.min(halfX, halfY) && pathX >= 2 * grid && pathY >= 2 * grid,
      t('operations.check.chamferRoom'))
  }
  const centreEntry = c.inside && c.shape === 'circle'
  const lead = centreEntry ? pathY / 2 : ceil(topRadius - offset) + grid
  if (c.inside && c.shape === 'rectangle')
    checks.check(lead < halfX - corner && lead + topRadius < halfX
      && lead < pathY, t('operations.check.chamferRoom'))
  if (checks.errors.length) return { errors: checks.errors }

  const start: Point = [0, -pathY]
  const leadCentre: Point = [0, -pathY + side * lead]
  const entry: Point = centreEntry ? [0, 0] : [-side * lead, leadCentre[1]]
  const exit: Point = centreEntry ? entry : [side * lead, leadCentre[1]]
  const p = new Program(c)
  const x = round(c.x)
  const y = round(c.y)
  const xy = ([dx, dy]: Point): string => `X${p.n(x + dx)} Y${p.n(y + dy)}`
  const arc = (code: string, from: Point, to: Point, centre: Point): void => {
    p.cut(`${code} ${xy(to)} I${p.n(centre[0] - from[0])} J${p.n(centre[1] - from[1])}`, c.feed)
  }
  const mirror = ([a, b]: Point): Point => [side * a, b]
  const outline: Segment[] = c.shape === 'circle' ? [{ end: start, centre: [0, 0] }]
    : rectangle(pathX, pathY, pathCorner)
  p.start('Chamfer', [ // i18n-ignore
    `${c.inside ? 'Inside' : 'Outside'} ${c.shape}, width ${p.n(c.chamferWidth)} on the top face`, // i18n-ignore
    `Included angle ${String(c.includedAngle)} degrees, tip diameter ${p.n(c.tipDiameter)}, tool tip Z-${p.n(tipDepth)}`, // i18n-ignore
    `Keep the entry clear at ${xy(entry)}`, // i18n-ignore
  ])
  p.add('G91.1', `G0 ${xy(entry)}`)
  p.cut(`G1 Z${p.n(-tipDepth)}`, c.entryFeed)
  arc('G3', entry, start, leadCentre)
  let from = start
  for (const segment of outline) {
    const to = mirror(segment.end)
    if (segment.centre) arc(c.inside ? 'G3' : 'G2', from, to, mirror(segment.centre))
    else p.cut(`G1 ${xy(to)}`, c.feed)
    from = to
  }
  arc('G3', start, exit, leadCentre)
  return { text: p.end() }
}
