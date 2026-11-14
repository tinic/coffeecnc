import { describe, expect, it } from 'vitest'

import { basis, boxCorners, boxEdges, fit, orbit, pan, project, views, zoom, type Camera } from '../src/pathview'
import type { Vec3 } from '../src/protocol'

const width = 800
const height = 600

function camera(view: keyof typeof views): Camera {
  return { ...views[view], scale: 2, centre: [0, 0, 0] }
}

function rounded(p: [number, number]): number[] {
  return p.map((v) => Math.round(v * 1000) / 1000 + 0)
}

describe('projection', () => {
  it('top view: X to the right, Y up, Z towards the viewer', () => {
    const cam = camera('top')
    expect(rounded(project(cam, [0, 0, 0], width, height))).toEqual([400, 300])
    expect(rounded(project(cam, [10, 0, 0], width, height))).toEqual([420, 300])
    expect(rounded(project(cam, [0, 10, 0], width, height))).toEqual([400, 280])
    expect(rounded(project(cam, [0, 0, 10], width, height))).toEqual([400, 300])
  })

  it('front view: X to the right, Z up, Y away from the viewer', () => {
    const cam = camera('front')
    expect(rounded(project(cam, [10, 0, 0], width, height))).toEqual([420, 300])
    expect(rounded(project(cam, [0, 0, 10], width, height))).toEqual([400, 280])
    expect(rounded(project(cam, [0, 10, 0], width, height))).toEqual([400, 300])
  })

  it('isometric view: Z up, X and Y both visible', () => {
    const cam = camera('iso')
    const [zx, zy] = project(cam, [0, 0, 10], width, height)
    expect(zx).toBeCloseTo(400, 6)
    expect(zy).toBeLessThan(300)
    expect(project(cam, [10, 0, 0], width, height)[0]).toBeGreaterThan(400)
    expect(project(cam, [0, 10, 0], width, height)[1]).toBeLessThan(300)
  })

  it('right and up are perpendicular unit vectors', () => {
    for (const name of ['iso', 'top', 'front'] as const) {
      const { right, up } = basis(camera(name))
      expect(Math.hypot(...right)).toBeCloseTo(1, 12)
      expect(Math.hypot(...up)).toBeCloseTo(1, 12)
      expect(right[0] * up[0] + right[1] * up[1] + right[2] * up[2]).toBeCloseTo(0, 12)
    }
  })
})

describe('camera', () => {
  const box = boxCorners([100, 50, -40], [300, 150, 0])

  it('fit brings every point inside the margin, in every view', () => {
    for (const name of ['iso', 'top', 'front'] as const) {
      const cam = fit(camera(name), box, width, height, 0.1)
      const screen = box.map((p) => project(cam, p, width, height))
      const xs = screen.map((p) => p[0])
      const ys = screen.map((p) => p[1])
      expect(Math.min(...xs)).toBeGreaterThanOrEqual(width * 0.1 - 1e-6)
      expect(Math.max(...xs)).toBeLessThanOrEqual(width * 0.9 + 1e-6)
      expect(Math.min(...ys)).toBeGreaterThanOrEqual(height * 0.1 - 1e-6)
      expect(Math.max(...ys)).toBeLessThanOrEqual(height * 0.9 + 1e-6)
      // Centred, and as large as the margin allows in one direction.
      expect((Math.min(...xs) + Math.max(...xs)) / 2).toBeCloseTo(width / 2, 6)
      expect((Math.min(...ys) + Math.max(...ys)) / 2).toBeCloseTo(height / 2, 6)
      const fills =
        Math.abs(Math.max(...xs) - Math.min(...xs) - width * 0.8) < 1e-6 ||
        Math.abs(Math.max(...ys) - Math.min(...ys) - height * 0.8) < 1e-6
      expect(fills).toBe(true)
    }
  })

  it('fit puts the pivot in the middle of the points, so that orbiting keeps them in view', () => {
    const cam = fit(camera('iso'), box, width, height)
    expect(cam.centre.map((v) => Math.round(v * 1000) / 1000 + 0)).toEqual([200, 100, -20])
    const turned = orbit(cam, 300, -80)
    for (const p of box) {
      const [x, y] = project(turned, p, width, height)
      expect(x).toBeGreaterThan(0)
      expect(x).toBeLessThan(width)
      expect(y).toBeGreaterThan(0)
      expect(y).toBeLessThan(height)
    }
  })

  it('fit leaves the camera alone without points or without a canvas', () => {
    const cam = camera('iso')
    expect(fit(cam, [], width, height)).toBe(cam)
    expect(fit(cam, box, 0, 0)).toBe(cam)
  })

  it('fit copes with a single point', () => {
    const cam = fit(camera('top'), [[5, 5, 5]], width, height)
    expect(Number.isFinite(cam.scale)).toBe(true)
    expect(rounded(project(cam, [5, 5, 5], width, height))).toEqual([400, 300])
  })

  it('zoom keeps the point under the cursor in place', () => {
    const cam = camera('iso')
    const p: Vec3 = [30, -20, 10]
    const at = project(cam, p, width, height)
    const zoomed = zoom(cam, 1.7, at, width, height)
    expect(zoomed.scale).toBeCloseTo(3.4, 12)
    expect(rounded(project(zoomed, p, width, height))).toEqual(rounded(at))
  })

  it('zoom stays within sane limits', () => {
    expect(zoom(camera('top'), 1e9, [0, 0], width, height).scale).toBe(400)
    expect(zoom(camera('top'), 1e-9, [0, 0], width, height).scale).toBe(0.05)
  })

  it('pan moves the picture by the pixels dragged', () => {
    const cam = camera('iso')
    const before = project(cam, [10, 20, 30], width, height)
    const after = project(pan(cam, 25, -40), [10, 20, 30], width, height)
    expect(after[0] - before[0]).toBeCloseTo(25, 9)
    expect(after[1] - before[1]).toBeCloseTo(-40, 9)
  })

  it('orbit turns the view and stops at the top and front views', () => {
    const cam = orbit(camera('iso'), 100, 0)
    expect(cam.azimuth).toBeCloseTo(views.iso.azimuth + 0.8, 12)
    expect(orbit(camera('iso'), 0, 10_000).elevation).toBe(Math.PI / 2)
    expect(orbit(camera('iso'), 0, -10_000).elevation).toBe(0)
  })

  it('a box has eight corners and twelve edges, each along one axis', () => {
    expect(box).toHaveLength(8)
    expect(boxEdges).toHaveLength(12)
    for (const [a, b] of boxEdges) {
      const from = box[a] ?? [0, 0, 0]
      const to = box[b] ?? [0, 0, 0]
      expect([0, 1, 2].filter((axis) => from[axis] !== to[axis])).toHaveLength(1)
    }
  })
})
