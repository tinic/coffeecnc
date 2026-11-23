// Picking a move on the path view and finding the moves of program lines, for the link between
// the path view and the program text.

import { describe, expect, it } from 'vitest'

import { buildPickIndex, distanceToSegmentSquared, isTap, pickRadius, pickSegment, rapidPenalty, segmentsOfLines } from '../src/path-pick'
import { fit, project, views, type Camera, type Screen } from '../src/pathview'
import type { PreviewSegment, Vec3 } from '../src/protocol'
import { splitLines } from '../src/sim/files'
import { newModal, previewProgram } from '../src/sim/gcode'
import pocket from '../src/sim/programs/plate-face-pocket.ngc?raw'
import button from '../../samples/upstream/kroneum/hw/enclosure/Rev_0.3/cnc/button.nc?raw'

const tools = Array.from({ length: 8 }, (_, i) => ({ number: i + 1, pocket: i + 1, diameter: 6, length: 40, comment: '' }))
const width = 800
const height = 500

function segment(line: number, kind: PreviewSegment['kind'], ...points: Vec3[]): PreviewSegment {
  return { line, kind, tool: 1, feed: kind === 'feed' ? 600 : 0, points: points.flat() }
}

/** Looking down, 4 px per mm, machine X0 Y0 in the middle of the canvas. */
const top: Camera = { ...views.top, scale: 4, centre: [0, 0, 0] }
const screen = (p: Vec3, camera = top): Screen => project(camera, p, width, height)
const near = (p: Vec3, dx: number, dy: number, camera = top): Screen => {
  const [x, y] = screen(p, camera)
  return [x + dx, y + dy]
}

describe('lines and segments', () => {
  it('finds every move of a line, also a line the program repeats', () => {
    const segments = [segment(5, 'rapid', [0, 0, 5], [10, 0, 5]), segment(6, 'feed', [10, 0, 5], [10, 0, -1]),
      segment(7, 'feed', [10, 0, -1], [20, 0, -1]), segment(6, 'feed', [20, 0, -1], [20, 0, -2])]
    expect(segmentsOfLines(segments, 6, 6)).toEqual([1, 3])
    expect(segmentsOfLines(segments, 5, 7)).toEqual([0, 1, 2, 3])
    expect(segmentsOfLines(segments, 8, 9)).toEqual([])
    expect(segmentsOfLines(segments, 0, 0)).toEqual([])
    expect(segmentsOfLines(segments, 7, 6)).toEqual([])
  })

  it('keeps the source line of each segment of a real program in the index', () => {
    const preview = previewProgram(newModal(tools), splitLines(pocket), false, 1)
    expect(preview.error).toBeNull()
    const index = buildPickIndex(preview.segments)
    expect(Array.from(index.line)).toEqual(preview.segments.map((s) => s.line))
    expect(index.start.at(-1)).toBe(preview.segments.reduce((sum, s) => sum + s.points.length / 3, 0))
  })
})

describe('picking', () => {
  it('measures the distance to a screen segment, end points included', () => {
    expect(distanceToSegmentSquared(5, 3, 0, 0, 10, 0)).toBe(9)
    expect(distanceToSegmentSquared(-3, 4, 0, 0, 10, 0)).toBe(25)
    expect(distanceToSegmentSquared(2, 2, 1, 1, 1, 1)).toBe(2)
  })

  it('picks the move under a finger-sized tap, and nothing beyond the radius', () => {
    const index = buildPickIndex([segment(10, 'feed', [0, 0, 0], [20, 0, 0])])
    expect(pickSegment(index, top, near([10, 0, 0], 0, 0), width, height)).toEqual({ segment: 0, line: 10, distance: 0 })
    expect(pickSegment(index, top, near([10, 0, 0], 0, pickRadius - 1), width, height)?.line).toBe(10)
    expect(pickSegment(index, top, near([20, 0, 0], pickRadius - 1, 0), width, height)?.line).toBe(10)
    expect(pickSegment(index, top, near([10, 0, 0], 0, pickRadius + 1), width, height)).toBeNull()
  })

  it('prefers a nearby cut to a rapid, unless the rapid is clearly nearer', () => {
    const rapid = segment(1, 'rapid', [0, 0, 0], [20, 0, 0])
    // The rapid under the finger, a cut 8 px away: the cut wins.
    const close = buildPickIndex([rapid, segment(2, 'feed', [0, 2, 0], [20, 2, 0])])
    expect(pickSegment(close, top, near([10, 0, 0], 0, 0), width, height)?.line).toBe(2)
    // A cut 20 px away, more than the rapid penalty: the rapid under the finger wins.
    const apart = buildPickIndex([rapid, segment(2, 'feed', [0, 5, 0], [20, 5, 0])])
    expect(pickSegment(apart, top, near([10, 0, 0], 0, 0), width, height)?.line).toBe(1)
    // Halfway between them the cut is less than the penalty farther: the cut wins.
    expect(pickSegment(apart, top, near([10, 0, 0], 0, -10), width, height)?.line).toBe(2)
    expect(rapidPenalty).toBeLessThan(20)
    // A rapid alone is picked.
    expect(pickSegment(buildPickIndex([segment(1, 'rapid', [0, 0, 0], [20, 0, 0])]), top, near([10, 0, 0], 0, 3), width, height)?.line).toBe(1)
  })

  it('picks the later of two moves on the same place, which is drawn on top', () => {
    const index = buildPickIndex([segment(3, 'feed', [0, 0, -1], [20, 0, -1]), segment(9, 'feed', [0, 0, -2], [20, 0, -2])])
    expect(pickSegment(index, top, near([10, 0, 0], 0, 0), width, height)?.line).toBe(9)
  })

  it('picks along tessellated arcs and in a turned view', () => {
    const arc: Vec3[] = Array.from({ length: 33 }, (_, i) => [10 * Math.cos(i * Math.PI / 16), 10 * Math.sin(i * Math.PI / 16), -1])
    const index = buildPickIndex([segment(4, 'feed', ...arc), segment(5, 'feed', [0, 0, -1], [0, 0, -10])])
    const iso: Camera = { ...views.iso, scale: 4, centre: [0, 0, -5] }
    expect(pickSegment(index, iso, near([0, 10, -1], 0, 0, iso), width, height)?.line).toBe(4)
    expect(pickSegment(index, iso, near([0, 0, -6], 2, 0, iso), width, height)?.line).toBe(5)
  })

  it('ignores segments without points and an empty preview', () => {
    expect(pickSegment(buildPickIndex([]), top, [400, 250], width, height)).toBeNull()
    expect(pickSegment(buildPickIndex([segment(1, 'feed')]), top, [400, 250], width, height)).toBeNull()
  })

  it('finds each cut of a real program where it is drawn', () => {
    const preview = previewProgram(newModal(tools), splitLines(pocket), false, 1)
    const camera = fit({ ...views.iso, scale: 1, centre: [0, 0, 0] }, preview.segments.flatMap((s) => {
      const points: Vec3[] = []
      for (let i = 0; i + 2 < s.points.length; i += 3) points.push([s.points[i] ?? 0, s.points[i + 1] ?? 0, s.points[i + 2] ?? 0])
      return points
    }), width, height)
    const index = buildPickIndex(preview.segments)
    for (const [number, s] of preview.segments.entries()) {
      if (s.kind !== 'feed' || s.points.length < 6) continue
      const a: Vec3 = [s.points[0] ?? 0, s.points[1] ?? 0, s.points[2] ?? 0]
      const b: Vec3 = [s.points[3] ?? 0, s.points[4] ?? 0, s.points[5] ?? 0]
      const pick = pickSegment(index, camera, screen([(a[0] + b[0]) / 2, (a[1] + b[1]) / 2, (a[2] + b[2]) / 2], camera), width, height)
      // The tap is on this move; another move on the same pixels may win, but only on them.
      expect(pick?.distance ?? Infinity, `segment ${String(number)}`).toBeLessThan(0.5)
    }
  })
})

describe('taps and drags', () => {
  it('treats a small movement as a tap and a larger one as a drag', () => {
    expect(isTap([100, 100], [104, 106])).toBe(true)
    expect(isTap([100, 100], [100, 111])).toBe(false)
  })
})

describe('picking in a large program', () => {
  it('stays fast in the 14,214-line kroneum button program', () => {
    // The simulator's subset: comments (one is nested) and G64 out; the moves are unchanged.
    const lines = splitLines(button).map((line) => line.replace(/\(.*\)/, '').replace(/\bG64\b/, ''))
    expect(lines.length).toBe(14214)
    const preview = previewProgram(newModal(tools), lines, false, 1)
    expect(preview.error).toBeNull()
    const points = preview.segments.reduce((sum, s) => sum + s.points.length / 3, 0)
    let started = performance.now()
    const index = buildPickIndex(preview.segments)
    const build = performance.now() - started
    const all: Vec3[] = []
    for (let i = 0; i < index.points.length; i += 3) all.push([index.points[i] ?? 0, index.points[i + 1] ?? 0, index.points[i + 2] ?? 0])
    const camera = fit({ ...views.iso, scale: 1, centre: [0, 0, 0] }, all, 1280, 700)
    // Taps all over the canvas, half of them on the path.
    const taps: Screen[] = []
    for (let i = 0; i < 400; i += 1) {
      const p = all[(i * 7919) % all.length] ?? [0, 0, 0]
      taps.push(i % 2 === 0 ? project(camera, p, 1280, 700) : [(i * 37) % 1280, (i * 53) % 700])
    }
    const times: number[] = []
    let hits = 0
    for (const tap of taps) {
      started = performance.now()
      if (pickSegment(index, camera, tap, 1280, 700) !== null) hits += 1
      times.push(performance.now() - started)
    }
    // The first pick includes compiling the loop; the operator's first tap pays that once.
    const first = times[0] ?? 0
    times.sort((a, b) => a - b)
    const median = times[Math.floor(times.length / 2)] ?? 0
    const p95 = times[Math.floor(times.length * 0.95)] ?? 0
    // Shown with `npx vitest run test/path-pick.test.ts --silent=false`.
    console.warn(`pick-perf lines=${String(lines.length)} segments=${String(preview.segments.length)} points=${String(points)} ` +
      `build_ms=${build.toFixed(2)} first_ms=${first.toFixed(3)} median_ms=${median.toFixed(3)} p95_ms=${p95.toFixed(3)} ` +
      `hits=${String(hits)}/${String(taps.length)}`)
    expect(hits).toBeGreaterThanOrEqual(200)
    // A frame at 60 Hz is 16.7 ms; a tap must answer well inside it, also on a slow panel.
    expect(median).toBeLessThan(4)
    expect(p95).toBeLessThan(16)
    expect(build).toBeLessThan(200)
  })
})
