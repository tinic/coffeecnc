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
