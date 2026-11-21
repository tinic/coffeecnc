import { describe, expect, it } from 'vitest'

import { cycleTime, formatDuration, progressAt, remainingFrom, sectionSeconds, velocityLimits, type VelocityLimits } from '../src/cycle-time'
import type { Hello, Preview, PreviewSegment } from '../src/protocol'
import { rig } from './harness'

// The G0759: 1500 mm/min in X, Y and along the path, 750 mm/min in Z.
const g0759: VelocityLimits = { path: 1500, axes: [1500, 1500, 750] }

function segment(line: number, kind: 'rapid' | 'feed', feed: number, ...points: number[]): PreviewSegment {
  return { line, kind, tool: 1, points, feed }
}
function preview(segments: PreviewSegment[], dwells: Preview['dwells'] = []): Preview {
  return { type: 'preview', context: 1, state: 'ready', error: null, segments, bounds: null, dwells }
}

describe('cycle time', () => {
  it('runs feeds at their programmed feed and rapids at the axis limits', () => {
    const time = cycleTime(preview([
      // 40 mm in X at 600 mm/min: 4 s.
      segment(1, 'feed', 600, 0, 0, 0, 40, 0, 0),
      // 25 mm in Z as a rapid: Z is limited to 750 mm/min, 2 s.
      segment(2, 'rapid', 0, 40, 0, 0, 40, 0, 25),
      // A feed above the machine's path limit runs at 1500: 30 mm in 1.2 s.
      segment(3, 'feed', 5000, 40, 0, 25, 40, 30, 25),
    ]), g0759)
    expect(time.total).toBeCloseTo(4 + 2 + 1.2, 6)
    expect(time.timeline.map((entry) => [entry.line, entry.start])).toEqual([[1, 0], [2, 4], [3, 6]])
    expect(time.unknownFeeds).toBe(false)
  })

  it('holds a diagonal move back to its slowest axis', () => {
    // 30 mm in X and 40 mm down in Z at F1500: Z would need 1200 mm/min, so the path slows to
    // 750 × 50 / 40 = 937.5 mm/min and 50 mm take 3.2 s.
    const time = cycleTime(preview([segment(1, 'feed', 1500, 0, 0, 40, 30, 0, 0)]), g0759)
    expect(time.total).toBeCloseTo(3.2, 6)
  })

  it('adds dwells after the moves of their line and counts tool changes apart', () => {
    const t2 = { ...segment(5, 'feed', 600, 0, 0, 0, 10, 0, 0), tool: 2 }
    const time = cycleTime(preview([segment(1, 'feed', 600, 0, 0, 0, 10, 0, 0), t2], [{ line: 3, seconds: 2 }]), g0759)
    expect(time.timeline.map((entry) => [entry.line, entry.seconds])).toEqual([[1, 1], [3, 2], [5, 1]])
    expect(time.total).toBe(4)
    expect(time.toolChanges).toBe(1)
    expect(remainingFrom(time, progressAt(time, 3))).toBe(3)
    expect(remainingFrom(time, progressAt(time, 5))).toBe(1)
    expect(remainingFrom(time, progressAt(time, 6))).toBe(0)
    expect(sectionSeconds(time, [{ start: 0 }, { start: 1 }])).toEqual([3, 1])
  })

  it('follows a subroutine called twice and counts it in the section that calls it', () => {
    // Lines 20 to 21 are a subroutine at the end of the file: T1 calls it from line 3, T2 from
    // line 7. Each move takes 1 s.
    const move = (line: number, tool: number): PreviewSegment => ({ ...segment(line, 'feed', 600, 0, 0, 0, 10, 0, 0), tool })
    const time = cycleTime(preview([move(2, 1), move(20, 1), move(21, 1), move(6, 2), move(20, 2), move(21, 2), move(8, 2)]), g0759)
    expect(sectionSeconds(time, [{ start: 0 }, { start: 3 }])).toEqual([3, 4])
    // In the second call the machine reports line 20 again: 3 s are left, not the 6 s from the first call.
    const first = progressAt(time, 20)
    expect(remainingFrom(time, first)).toBe(6)
    const second = progressAt(time, 20, progressAt(time, 6, first))
    expect(remainingFrom(time, second)).toBe(3)
    expect(remainingFrom(time, progressAt(time, 21, second))).toBe(2)
  })

  it('says when a feed was not per minute and counts it at full speed', () => {
    const time = cycleTime(preview([segment(1, 'feed', 0, 0, 0, 0, 25, 0, 0)]), g0759)
    expect(time.unknownFeeds).toBe(true)
    expect(time.total).toBeCloseTo(1, 6)
  })

  it('takes the limits from Hello, never above the path maximum', () => {
    const hello = { max_jog_velocity: 1500, axis_max_jog_velocity: { x: 1500, y: 2000, z: 750 } } as Hello
    expect(velocityLimits(hello)).toEqual({ path: 1500, axes: [1500, 1500, 750] })
    expect(velocityLimits(null)).toBeNull()
  })

  it('formats minutes and hours', () => {
    expect(formatDuration(75)).toBe('1:15')
    expect(formatDuration(3725)).toBe('1:02:05')
    expect(formatDuration(-3)).toBe('0:00')
  })

  it('gets feeds and dwells from the simulator preview', () => {
    const r = rig()
    r.home()
    r.load('G21 G90\nG0 Z5\nG1 Z0 F120\nG4 P1.5\nG1 X10 F600\nM30\n')
    const result = r.preview()
    expect(result.segments.map((s) => [s.line, s.kind, s.feed])).toEqual([[2, 'rapid', 0], [3, 'feed', 120], [5, 'feed', 600]])
    expect(result.dwells).toEqual([{ line: 4, seconds: 1.5 }])
  })
})
