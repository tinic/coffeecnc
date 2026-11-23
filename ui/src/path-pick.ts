// Links the path view and the program text: which drawn segments come from which source lines,
// and which segment a tap on the path view means. Pure functions, so they can be tested without a
// canvas.

import { basis, type Camera, type Screen } from './pathview'
import type { PreviewSegment } from './protocol'

/** A finger tap lands within this distance (CSS pixels) of the move it means. */
export const pickRadius = 24
/** A rapid must be this much nearer (CSS pixels) than a cut to be picked instead. */
export const rapidPenalty = 12
/** A pointer that moves farther than this (CSS pixels) is a drag, not a tap. */
export const tapSlop = 10

/**
 * The preview's polylines in flat arrays, with a bounding sphere for each segment. The camera
 * turns in three dimensions, so the index is in machine space; a pick projects only the segments
 * whose sphere comes near the tap.
 */
export interface PickIndex {
  /** x, y, z of every point of every segment. */
  points: Float64Array
  /** Segment `i` has points `start[i]` to `start[i + 1]` (exclusive), counted in points. */
  start: Uint32Array
  /** Centre x, y, z and radius of each segment's bounding sphere. */
  spheres: Float64Array
  rapid: Uint8Array
  line: Int32Array
}

export function buildPickIndex(segments: readonly PreviewSegment[]): PickIndex {
  let total = 0
  for (const segment of segments) total += Math.floor(segment.points.length / 3)
  const points = new Float64Array(total * 3)
  const start = new Uint32Array(segments.length + 1)
  const spheres = new Float64Array(segments.length * 4)
  const rapid = new Uint8Array(segments.length)
  const line = new Int32Array(segments.length)
  let at = 0
  segments.forEach((segment, index) => {
    start[index] = at / 3
    rapid[index] = segment.kind === 'rapid' ? 1 : 0
    line[index] = segment.line
    const p = segment.points
    const count = Math.floor(p.length / 3) * 3
    let lowX = Infinity, lowY = Infinity, lowZ = Infinity
    let highX = -Infinity, highY = -Infinity, highZ = -Infinity
    for (let i = 0; i < count; i += 3) {
      const x = p[i] ?? 0, y = p[i + 1] ?? 0, z = p[i + 2] ?? 0
      points[at++] = x
      points[at++] = y
      points[at++] = z
      if (x < lowX) lowX = x
      if (x > highX) highX = x
      if (y < lowY) lowY = y
      if (y > highY) highY = y
      if (z < lowZ) lowZ = z
      if (z > highZ) highZ = z
    }
    const s = index * 4
    if (count === 0) {
      spheres[s + 3] = -1
      return
    }
    spheres[s] = (lowX + highX) / 2
    spheres[s + 1] = (lowY + highY) / 2
    spheres[s + 2] = (lowZ + highZ) / 2
    spheres[s + 3] = Math.hypot(highX - lowX, highY - lowY, highZ - lowZ) / 2
  })
  start[segments.length] = at / 3
  return { points, start, spheres, rapid, line }
}

/** Squared distance from (px, py) to the screen segment (ax, ay)–(bx, by). */
export function distanceToSegmentSquared(px: number, py: number, ax: number, ay: number, bx: number, by: number): number {
  const dx = bx - ax
  const dy = by - ay
  const length = dx * dx + dy * dy
  let t = length > 0 ? ((px - ax) * dx + (py - ay) * dy) / length : 0
  t = t < 0 ? 0 : t > 1 ? 1 : t
  const x = ax + t * dx - px
  const y = ay + t * dy - py
  return x * x + y * y
}

export interface Pick {
  /** Index into the preview's segments. */
  segment: number
  line: number
  /** Screen distance from the tap, CSS pixels. */
  distance: number
}

/**
 * The segment a tap at `at` means: the nearest within `radius` on the screen, where a rapid must
 * be `penalty` nearer than a cut to win. Of equal candidates the later one wins, because it is
 * drawn on top. Null when nothing is near.
 */
export function pickSegment(index: PickIndex, camera: Camera, at: Screen, width: number, height: number,
  radius = pickRadius, penalty = rapidPenalty): Pick | null {
  const { right, up } = basis(camera)
  const [rx, ry, rz] = right
  const [ux, uy, uz] = up
  const scale = camera.scale
  const [cx, cy, cz] = camera.centre
  // Screen x = ox + p·right·scale, screen y = oy − p·up·scale.
  const ox = width / 2 - (cx * rx + cy * ry + cz * rz) * scale
  const oy = height / 2 + (cx * ux + cy * uy + cz * uz) * scale
  const [px, py] = at
  const { points, start, spheres, rapid, line } = index
  const segments = line.length
  let best = -1
  let bestScore = Infinity
  let bestDistance = Infinity
  for (let s = 0; s < segments; s += 1) {
    const sphere = s * 4
    const sphereRadius = spheres[sphere + 3] ?? -1
    if (sphereRadius < 0) continue
    const extra = rapid[s] === 1 ? penalty : 0
    // Orthographic: the sphere projects to a circle of radius sphereRadius·scale.
    const sx = ox + ((spheres[sphere] ?? 0) * rx + (spheres[sphere + 1] ?? 0) * ry + (spheres[sphere + 2] ?? 0) * rz) * scale
    const sy = oy - ((spheres[sphere] ?? 0) * ux + (spheres[sphere + 1] ?? 0) * uy + (spheres[sphere + 2] ?? 0) * uz) * scale
    const reach = Math.min(radius, bestScore - extra) + sphereRadius * scale
    if (reach < 0 || (sx - px) * (sx - px) + (sy - py) * (sy - py) > reach * reach) continue
    const first = start[s] ?? 0
    const last = start[s + 1] ?? 0
    let nearest = Infinity
    let ax = 0, ay = 0
    for (let i = first; i < last; i += 1) {
      const x = points[i * 3] ?? 0, y = points[i * 3 + 1] ?? 0, z = points[i * 3 + 2] ?? 0
      const bx = ox + (x * rx + y * ry + z * rz) * scale
      const by = oy - (x * ux + y * uy + z * uz) * scale
      const d = i === first ? (bx - px) * (bx - px) + (by - py) * (by - py) : distanceToSegmentSquared(px, py, ax, ay, bx, by)
      if (d < nearest) nearest = d
      ax = bx
      ay = by
    }
    const distance = Math.sqrt(nearest)
    const score = distance + extra
    if (distance <= radius && score <= bestScore) {
      best = s
      bestScore = score
      bestDistance = distance
    }
  }
  return best < 0 ? null : { segment: best, line: line[best] ?? 0, distance: bestDistance }
}

/** The segments that come from source lines `first` to `last`, inclusive. */
export function segmentsOfLines(segments: readonly PreviewSegment[], first: number, last: number): number[] {
  const found: number[] = []
  if (first <= 0 || last < first) return found
  segments.forEach((segment, index) => { if (segment.line >= first && segment.line <= last) found.push(index) })
  return found
}

/** True when a pointer that went down at `from` and up at `to` has not moved far enough to drag. */
export function isTap(from: Screen, to: Screen, slop = tapSlop): boolean {
  return Math.hypot(to[0] - from[0], to[1] - from[1]) <= slop
}
