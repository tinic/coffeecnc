// The path view's camera: an orthographic projection with orbit, pan and zoom. Pure functions, so
// the picture can be tested without a canvas.

import type { Vec3 } from './protocol'

export interface Camera {
  /** Rotation about Z, radians. */
  azimuth: number
  /** Angle above the XY plane, radians: 0 looks along Y (front), π/2 looks down (top). */
  elevation: number
  /** Pixels per millimetre. */
  scale: number
  /** The point shown at the middle of the canvas. */
  centre: Vec3
}

export type Screen = [number, number]

export type ViewName = 'iso' | 'top' | 'front'

export const views: Readonly<Record<ViewName, { azimuth: number; elevation: number }>> = {
  iso: { azimuth: (-35 * Math.PI) / 180, elevation: (35 * Math.PI) / 180 },
  top: { azimuth: 0, elevation: Math.PI / 2 },
  front: { azimuth: 0, elevation: 0 },
}

/** Feed colours by order of appearance of the tools in the program. */
export const toolColours: readonly string[] = ['#58b8ae', '#d6a35c', '#8ea4e6', '#c78fbb', '#9cc36e', '#de8c7c']

export function toolColour(index: number): string {
  return toolColours[index % toolColours.length] ?? '#58b8ae'
}

/** The world directions that point right and up on the screen. */
export function basis(camera: Camera): { right: Vec3; up: Vec3 } {
  const ca = Math.cos(camera.azimuth)
  const sa = Math.sin(camera.azimuth)
  const ce = Math.cos(camera.elevation)
  const se = Math.sin(camera.elevation)
  return { right: [ca, sa, 0], up: [-sa * se, ca * se, ce] }
}

function dot(a: Vec3, b: Vec3): number {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}

export function project(camera: Camera, p: Vec3, width: number, height: number): Screen {
  const { right, up } = basis(camera)
  const d: Vec3 = [p[0] - camera.centre[0], p[1] - camera.centre[1], p[2] - camera.centre[2]]
  return [width / 2 + dot(d, right) * camera.scale, height / 2 - dot(d, up) * camera.scale]
}

/** The camera turned to show all of `points` with `margin` (a fraction of the canvas) around. */
export function fit(camera: Camera, points: Vec3[], width: number, height: number, margin = 0.12): Camera {
  if (points.length === 0 || width <= 0 || height <= 0) {
    return camera
  }
  const { right, up } = basis(camera)
  let minX = Infinity
  let maxX = -Infinity
  let minY = Infinity
  let maxY = -Infinity
  for (const p of points) {
    const x = dot(p, right)
    const y = dot(p, up)
    minX = Math.min(minX, x)
    maxX = Math.max(maxX, x)
    minY = Math.min(minY, y)
    maxY = Math.max(maxY, y)
  }
  const spanX = Math.max(maxX - minX, 1)
  const spanY = Math.max(maxY - minY, 1)
  const scale = Math.min((width * (1 - 2 * margin)) / spanX, (height * (1 - 2 * margin)) / spanY)
  const cx = (minX + maxX) / 2
  const cy = (minY + maxY) / 2
  // Any point with these screen coordinates will do; depth does not matter in this projection.
  return {
    ...camera,
    scale,
    centre: [right[0] * cx + up[0] * cy, right[1] * cx + up[1] * cy, right[2] * cx + up[2] * cy],
  }
}

export function orbit(camera: Camera, dx: number, dy: number): Camera {
  return {
    ...camera,
    azimuth: camera.azimuth + dx * 0.008,
    elevation: Math.min(Math.PI / 2, Math.max(0, camera.elevation + dy * 0.008)),
  }
}

export function pan(camera: Camera, dx: number, dy: number): Camera {
  const { right, up } = basis(camera)
  const x = -dx / camera.scale
  const y = dy / camera.scale
  return {
    ...camera,
    centre: [
      camera.centre[0] + right[0] * x + up[0] * y,
      camera.centre[1] + right[1] * x + up[1] * y,
      camera.centre[2] + right[2] * x + up[2] * y,
    ],
  }
}

/** Zooms by `factor`, keeping the point under screen position `at` where it is. */
export function zoom(camera: Camera, factor: number, at: Screen, width: number, height: number): Camera {
  const scale = Math.min(400, Math.max(0.05, camera.scale * factor))
  const sx = at[0] - width / 2
  const sy = height / 2 - at[1]
  const shift = 1 / camera.scale - 1 / scale
  const { right, up } = basis(camera)
  return {
    ...camera,
    scale,
    centre: [
      camera.centre[0] + (right[0] * sx + up[0] * sy) * shift,
      camera.centre[1] + (right[1] * sx + up[1] * sy) * shift,
      camera.centre[2] + (right[2] * sx + up[2] * sy) * shift,
    ],
  }
}

export function boxCorners(min: Vec3, max: Vec3): Vec3[] {
  const corners: Vec3[] = []
  for (const z of [min[2], max[2]]) {
    for (const y of [min[1], max[1]]) {
      for (const x of [min[0], max[0]]) {
        corners.push([x, y, z])
      }
    }
  }
  return corners
}

/** The twelve edges of a box, as index pairs into `boxCorners`. */
export const boxEdges: readonly (readonly [number, number])[] = [
  [0, 1],
  [2, 3],
  [4, 5],
  [6, 7],
  [0, 2],
  [1, 3],
  [4, 6],
  [5, 7],
  [0, 4],
  [1, 5],
  [2, 6],
  [3, 7],
]
