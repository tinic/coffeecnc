// How long a program takes, from its preview: each move at its programmed feed (rapids at the
// machine's fastest), capped by every axis's maximum velocity, plus the dwells. Acceleration and
// operator time (tool changes, pauses) are not in it, so it is an estimate that reads low.

import type { AxisLetter, Hello, Preview } from './protocol'

export interface VelocityLimits {
  /** Fastest path velocity, mm/min. */
  path: number
  /** Each axis's maximum velocity, mm/min. */
  axes: [number, number, number]
}

export function velocityLimits(hello: Hello | null): VelocityLimits | null {
  if (hello === null || !(hello.max_jog_velocity > 0)) return null
  const path = hello.max_jog_velocity
  const axis = (letter: AxisLetter): number => {
    const value = hello.axis_max_jog_velocity?.[letter]
    return value !== undefined && value > 0 ? Math.min(value, path) : path
  }
  return { path, axes: [axis('x'), axis('y'), axis('z')] }
}

/** Where in the program the time goes: `start` is the time when `line` begins. */
export interface TimelineEntry {
  line: number
  tool: number
  start: number
  seconds: number
}

export interface CycleTime {
  /** Seconds for the whole program at 100 % overrides. */
  total: number
  /** In program order; one entry per move run or dwell. */
  timeline: TimelineEntry[]
  /** Feed moves whose feed is not per minute (G93, G95) were counted at the fastest speed. */
  unknownFeeds: boolean
  /** Tool changes the operator makes, not counted in `total`. */
  toolChanges: number
}

function moveSeconds(points: number[], feed: number, limits: VelocityLimits): number {
  let seconds = 0
  for (let i = 3; i + 2 < points.length; i += 3) {
    const delta = [0, 1, 2].map((axis) => (points[i + axis] ?? 0) - (points[i - 3 + axis] ?? 0))
    const length = Math.hypot(...delta)
    if (length === 0) continue
    let velocity = Math.min(feed, limits.path)
    // An axis at its limit holds the whole move back.
    delta.forEach((d, axis) => {
      if (d !== 0) velocity = Math.min(velocity, limits.axes[axis as 0 | 1 | 2] * length / Math.abs(d))
    })
    seconds += length / velocity * 60
  }
  return seconds
}

export function cycleTime(preview: Preview, limits: VelocityLimits): CycleTime {
  const entries: Omit<TimelineEntry, 'start'>[] = []
  let unknownFeeds = false
  let toolChanges = 0
  let tool: number | null = null
  for (const segment of preview.segments) {
    if (tool !== null && segment.tool !== tool) toolChanges += 1
    tool = segment.tool
    const known = segment.kind === 'rapid' || segment.feed > 0
    if (!known) unknownFeeds = true
    const feed = segment.kind === 'rapid' || !known ? limits.path : segment.feed
    entries.push({ line: segment.line, tool: segment.tool, seconds: moveSeconds(segment.points, feed, limits) })
  }
  // Dwells go after the moves of earlier lines and of their own line.
  for (const dwell of preview.dwells) {
    let at = entries.findIndex((entry) => entry.line > dwell.line)
    if (at < 0) at = entries.length
    entries.splice(at, 0, { line: dwell.line, tool: entries[at - 1]?.tool ?? 0, seconds: dwell.seconds })
  }
  let start = 0
  const timeline = entries.map((entry) => {
    const placed = { ...entry, start }
    start += entry.seconds
    return placed
  })
  return { total: start, timeline, unknownFeeds, toolChanges }
}

/** Seconds of the program still to run when `line` is executing (0 when nothing is). */
export function remainingFrom(time: CycleTime, line: number): number {
  const current = time.timeline.find((entry) => entry.line >= line)
  return current === undefined ? 0 : time.total - current.start
}

/** Seconds spent in each tool section, keyed by the section's first line. */
export function sectionSeconds(time: CycleTime, sections: readonly { first: number; last: number }[]): number[] {
  return sections.map((section, index) => {
    const end = sections[index + 1]?.first ?? Infinity
    return time.timeline
      .filter((entry) => entry.line >= section.first && entry.line < end)
      .reduce((sum, entry) => sum + entry.seconds, 0)
  })
}

/** 75 s → "1:15", 3725 s → "1:02:05". */
export function formatDuration(seconds: number): string {
  const whole = Math.max(0, Math.round(seconds))
  const h = Math.floor(whole / 3600)
  const m = Math.floor(whole % 3600 / 60)
  const s = whole % 60
  const two = (n: number): string => String(n).padStart(2, '0')
  return h > 0 ? `${String(h)}:${two(m)}:${two(s)}` : `${String(m)}:${two(s)}`
}
