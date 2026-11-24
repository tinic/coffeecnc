// Three-point bore and boss geometry (#247), as the controller computes it
// (backend/src/core/include/coffeecnc/core/fixed_setter.hpp).

export type Angles = readonly [number, number, number]
type Point = readonly [number, number]

/** Fewer degrees than these between two directions, or a smaller arc, give a poor centre. */
export const arcMinSeparation = 10
export const arcMinSpan = 90
/** Contacts that cover less of the circle found from them lie almost on a line. */
export const arcMinMeasuredSpan = 90
/** The largest estimated centre uncertainty (95 %) a three-point cycle writes, mm. */
export const arcMaxCentreUncertainty = 0.02
/** One contact's radial standard deviation from the range of its three latch passes; never under 1 µm. */
export function contactSigma(latchSpread: number): number {
  return Math.max(latchSpread / 1.693, 0.001)
}

/**
 * The 95 % uncertainty of the centre of the circle through `points`, mm, for independent radial
 * errors of standard deviation `sigma` at each point; null when it has no estimate.
 */
export function arcCentreUncertainty(points: readonly [Point, Point, Point], centre: Point,
  sigma: readonly [number, number, number]): number | null {
  const n = points.map(([x, y]) => {
    const length = Math.hypot(x - centre[0], y - centre[1])
    return length > 0 ? [(x - centre[0]) / length, (y - centre[1]) / length] as const : null
  })
  const [a, b, c] = n
  if (!a || !b || !c) return null
  const det = a[0] * (b[1] - c[1]) - a[1] * (b[0] - c[0]) + (b[0] * c[1] - c[0] * b[1])
  if (!Number.isFinite(det) || Math.abs(det) < 1e-12) return null
  const gx = [(b[1] - c[1]) / det, (c[1] - a[1]) / det, (a[1] - b[1]) / det]
  const gy = [(c[0] - b[0]) / det, (a[0] - c[0]) / det, (b[0] - a[0]) / det]
  const variance = sigma.reduce((sum, s, i) => sum + ((gx[i] ?? 0) ** 2 + (gy[i] ?? 0) ** 2) * s * s, 0)
  const uncertainty = 1.96 * Math.sqrt(variance)
  return Number.isFinite(uncertainty) ? uncertainty : null
}

/** A three-point boss within the larger of 0.5 mm and 10 % of the entered diameter. */
export function bossDiameterPlausible(measured: number, entered: number): boolean {
  return Math.abs(measured - entered) <= Math.max(0.5, 0.1 * entered)
}

/** The smallest angle between two of the directions and the arc they cover (360° less the largest gap). */
export function arcSpread(degrees: Angles): { closest: number; span: number } {
  const [a, b, c] = degrees.map((angle) => ((angle % 360) + 360) % 360).sort((x, y) => x - y) as [number, number, number]
  const gaps = [b - a, c - b, 360 - c + a]
  return { closest: Math.min(...gaps), span: 360 - Math.max(...gaps) }
}

export function arcAnglesValid(degrees: Angles): boolean {
  if (!degrees.every(Number.isFinite)) return false
  const { closest, span } = arcSpread(degrees)
  return closest >= arcMinSeparation && span >= arcMinSpan
}

/** The circle through three points, or null when they lie (almost) on a line. */
export function circleThrough([p, q, r]: readonly [Point, Point, Point]): { centre: [number, number]; radius: number } | null {
  const [bx, by] = [q[0] - p[0], q[1] - p[1]]
  const [cx, cy] = [r[0] - p[0], r[1] - p[1]]
  const d = 2 * (bx * cy - by * cx)
  if (!Number.isFinite(d) || d === 0) return null
  const b2 = bx * bx + by * by
  const c2 = cx * cx + cy * cy
  const ux = (cy * b2 - by * c2) / d
  const uy = (bx * c2 - cx * b2) / d
  const radius = Math.hypot(ux, uy)
  return Number.isFinite(radius) && radius <= 5000 ? { centre: [p[0] + ux, p[1] + uy], radius } : null
}

/** The arc the points cover seen from `centre`, degrees. */
export function measuredSpan(points: readonly [Point, Point, Point], centre: Point): number {
  const [a, b, c] = points.map(([x, y]) => Math.atan2(y - centre[1], x - centre[0]) * 180 / Math.PI)
  return arcSpread([a ?? 0, b ?? 0, c ?? 0]).span
}


/** The fifth ray for a regular bore or boss. Angles are relative to work +X. */
export const circleRays = [0, 90, 180, 270] as const
export const circleCheckLimit = 0.02
export const circleMinResidualGain = 0.2
export const circleMaxCentreError = 0.05
export function checkAngleValid(angle: number, primary: readonly number[]): boolean {
  return Number.isFinite(angle) && primary.every(value => {
    const delta = ((angle - value) % 360 + 360) % 360
    return Math.min(delta, 360 - delta) >= arcMinSeparation
  })
}

export function largestGapMiddle(primary: readonly number[]): number {
  const degrees = primary.map(value => (value % 360 + 360) % 360).sort((a, b) => a - b)
  let largest = -1, middle = 45
  degrees.forEach((angle, index) => {
    const next = degrees[index + 1] ?? (degrees[0] ?? 0) + 360
    if (next - angle > largest) {
      largest = next - angle
      middle = (angle + largest / 2) % 360
    }
  })
  return middle
}

export function plannedCircleQuality(headings: readonly number[]): CircleQuality {
  const unit = headings.map(heading => {
    const angle = (heading % 360) * Math.PI / 180
    return [Math.cos(angle), Math.sin(angle)] as const
  })
  return circleQuality(unit, [])
}

export function measuredCircleQuality(points: readonly Point[], centre: Point): CircleQuality {
  const unit = points.map(([x, y]) => {
    const length = Math.hypot(x - centre[0], y - centre[1])
    return [(x - centre[0]) / length, (y - centre[1]) / length] as const
  })
  return circleQuality(unit, unit.map(() => 0))
}

interface CircleQuality { gain: number; uncertainty: number; centreError: number }
function circleQuality(unit: readonly Point[], spreads: readonly number[]): CircleQuality {
  const size = unit.length
  const [mx, my] = unit.reduce<Point>(([x, y], [u, v]) => [x + u / size, y + v / size], [0, 0])
  const rows = unit.map(([x, y]) => [x - mx, y - my] as const)
  let gain = 1, variance = 0, centreError = 0
  for (const [i, [x, y]] of rows.entries()) {
    const move = solve2(rows, rows.map((_, j) => Number(i === j)))
    if (!move) return { gain: 0, uncertainty: Infinity, centreError: Infinity }
    const ownResidual = 1 - 1 / size - x * move[0] - y * move[1]
    const centreGain = Math.hypot(...move)
    gain = Math.min(gain, ownResidual)
    centreError = Math.max(centreError, ownResidual > 0 ? circleCheckLimit * centreGain / ownResidual : Infinity)
    if (spreads.length > 0) variance += (contactSigma(spreads[i] ?? 0) * centreGain) ** 2
  }
  return { gain, uncertainty: 1.96 * Math.sqrt(variance), centreError }
}

interface CircleFit { centre: Point; radius: number; maxResidual: number }
interface CircleEvaluation extends CircleFit { sumSquares: number; step: Point | null }

function solve2(rows: readonly Point[], values: readonly number[]): Point | null {
  let xx = 0, xy = 0, yy = 0, bx = 0, by = 0
  for (const [i, [x, y]] of rows.entries()) {
    const value = values[i] ?? 0
    xx += x * x
    xy += x * y
    yy += y * y
    bx += x * value
    by += y * value
  }
  const det = xx * yy - xy * xy
  return Number.isFinite(det) && det > 1e-12 * (xx + yy) ** 2
    ? [(yy * bx - xy * by) / det, (xx * by - xy * bx) / det] : null
}

/** Fit the same radial residual as the controller, with the radius eliminated as a variable. */
export function fitProbeCircle(points: readonly Point[]): CircleFit | null {
  if (points.length < 4 || points.length > 5) return null
  const mean = (values: readonly number[]): number => values.reduce((sum, value) => sum + value / points.length, 0)
  const centroid = [mean(points.map(p => p[0])), mean(points.map(p => p[1]))] as const
  const rows = points.map(([x, y]) => [x - centroid[0], y - centroid[1]] as const)
  const seed = solve2(rows, rows.map(([x, y]) => (x * x + y * y) / 2))
  if (!seed) return null
  const evaluate = (centre: Point): CircleEvaluation | null => {
    const radial = points.map(([x, y]) => [x - centre[0], y - centre[1]] as const)
    const distances = radial.map(([x, y]) => Math.hypot(x, y))
    if (distances.some(distance => !(distance > 0) || !Number.isFinite(distance))) return null
    const radius = mean(distances)
    const errors = distances.map(distance => distance - radius)
    const unitX = radial.map(([x, y]) => x / Math.hypot(x, y))
    const unitY = radial.map(([x, y]) => y / Math.hypot(x, y))
    const mx = mean(unitX), my = mean(unitY)
    return { centre, radius, maxResidual: Math.max(...errors.map(Math.abs)),
      sumSquares: errors.reduce((sum, error) => sum + error * error, 0),
      step: solve2(unitX.map((x, i) => [x - mx, (unitY[i] ?? 0) - my]), errors) }
  }
  let fit = evaluate([centroid[0] + seed[0], centroid[1] + seed[1]])
  for (let i = 0; fit?.step && i < 30; i++) {
    const [dx, dy] = fit.step
    if (Math.hypot(dx, dy) <= 1e-8) {
      const { centre, radius, maxResidual } = fit
      return radius <= 5000 ? { centre, radius, maxResidual } : null
    }
    let improved = false
    for (let fraction = 1; fraction >= 1 / 128; fraction /= 2) {
      const next = evaluate([fit.centre[0] + fraction * dx, fit.centre[1] + fraction * dy])
      if (next && next.sumSquares < fit.sumSquares) {
        fit = next
        improved = true
        break
      }
    }
    if (!improved) return null
  }
  return null
}
