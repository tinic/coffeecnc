// Three-point bore and boss geometry (#247), as the controller computes it
// (backend/src/core/include/coffeecnc/core/fixed_setter.hpp).

export type Angles = readonly [number, number, number]
type Point = readonly [number, number]

/** Fewer degrees than these between two directions, or a smaller arc, give a poor centre. */
export const arcMinSeparation = 10
export const arcMinSpan = 30
/** Contacts that cover less of the circle found from them lie almost on a line. */
export const arcMinMeasuredSpan = 20

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
