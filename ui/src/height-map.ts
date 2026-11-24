// The last Z height map (#247): shown in the 3D view, saved as CSV and as a LinuxCNC probe file.
import { shallowRef } from 'vue'
import type { FixedSetterLimits, HeightMap, HeightMapPoint, LengthUnit } from './protocol'
import { toDisplay, wcsName } from './units'

/** The grid a height map probes, in work millimetres: corners, step and the clearance Z. */
export interface HeightMapGrid { x_min: number; y_min: number; x_max: number; y_max: number; step: number; clearance: number }

/** The map the 3D view draws, until another one replaces it or the operator hides it. */
export const heightMap = shallowRef<HeightMap | null>(null)

/** At most this many points, as natively (fixed_setter.hpp max_height_map_points). */
export const maxHeightMapPoints = 150
/** The longest a map may take, seconds, as natively (fixed_setter.hpp max_height_map_seconds). */
export const maxHeightMapSeconds = 7200

/** Columns and rows from the first corner to the second, both included, at most `step` apart. */
export function heightGrid(corners: readonly [number, number, number, number], step: number): { columns: number; rows: number } | null {
  if (!corners.every(Number.isFinite) || !Number.isFinite(step) || step < 0.5 || step > 500) return null
  const count = (from: number, to: number): number => Math.ceil(Math.abs(to - from) / step - 1e-9) + 1
  const [x0, y0, x1, y1] = corners
  if (Math.abs(x1 - x0) > 2000 || Math.abs(y1 - y0) > 2000) return null
  const grid = { columns: count(x0, x1), rows: count(y0, y1) }
  const points = grid.columns * grid.rows
  return points >= 2 && points <= maxHeightMapPoints ? grid : null
}

/**
 * The longest one point can take, seconds, as natively (fixed_setter.hpp height_map_point_seconds):
 * the search over the material travel, three latch passes, the retract and its release, and 10 s
 * for the lift, the traverse and the blocks. Infinite without the limits.
 */
export function heightMapPointSeconds(limits: FixedSetterLimits | undefined): number {
  const latch = limits?.latch_feed_mm_min ?? 0
  if (limits === undefined || !(limits.feed_mm_min > 0) || !(latch > 0)) return Infinity
  const travel = limits.material_travel_mm ?? 20
  const backoff = limits.latch_backoff_mm ?? 1
  const fast = 2 * limits.feed_mm_min
  const pass = backoff / fast * 60 + 2 + 2 * backoff / latch * 60
  return travel / limits.feed_mm_min * 60 + 3 * pass + travel / fast * 60 + 2 + 10
}

/** Every point, and the first feed down from the start to the clearance (`descent` mm). */
export function heightMapSeconds(limits: FixedSetterLimits | undefined, points: number, descent: number): number {
  const fast = 2 * (limits?.feed_mm_min ?? 0)
  return points * heightMapPointSeconds(limits) + Math.max(0, descent) / fast * 60
}

/** How long the panel waits for any other probe cycle's answer, ms. */
export const probeAnswerMs = 1_600_000

/**
 * How long the panel waits for a height map's answer, ms: its worst case and two minutes more,
 * and never less than for another probe cycle. `machineZ` is the start's machine Z and
 * `workOffsetZ` machine Z less work Z there, so the clearance's machine Z follows.
 */
export function heightMapAnswerMs(limits: FixedSetterLimits | undefined, grid: HeightMapGrid, machineZ: number, workOffsetZ: number): number {
  const size = heightGrid([grid.x_min, grid.y_min, grid.x_max, grid.y_max], grid.step)
  if (size === null) return probeAnswerMs
  const seconds = heightMapSeconds(limits, size.columns * size.rows, machineZ - (grid.clearance + workOffsetZ))
  return Number.isFinite(seconds) ? Math.max(probeAnswerMs, Math.ceil(seconds * 1000) + 120_000) : probeAnswerMs
}

/** The probing order: row by row, every other row from its far end. Grid indices. */
export function heightMapOrder(columns: number, rows: number): number[] {
  return Array.from({ length: columns * rows }, (_, step) => {
    const row = Math.floor(step / columns)
    const along = step % columns
    return row * columns + (row % 2 === 0 ? along : columns - 1 - along)
  })
}

/**
 * Which point of the probing order (1-based) the probe is over at work X, Y, mm: the point within
 * `tolerance` of it, or null between the points.
 */
export function heightMapPointAt(grid: HeightMapGrid, x: number, y: number, tolerance = 0.5): number | null {
  const size = heightGrid([grid.x_min, grid.y_min, grid.x_max, grid.y_max], grid.step)
  if (size === null) return null
  const at = (index: number, count: number, from: number, to: number): number => count === 1 ? from : from + (to - from) * index / (count - 1)
  const order = heightMapOrder(size.columns, size.rows)
  const index = order.findIndex((point) => {
    const px = at(point % size.columns, size.columns, grid.x_min, grid.x_max)
    const py = at(Math.floor(point / size.columns), size.rows, grid.y_min, grid.y_max)
    return Math.hypot(px - x, py - y) <= tolerance
  })
  return index < 0 ? null : index + 1
}

/** The points the map measured, in grid order. */
export function measuredPoints(map: HeightMap): HeightMapPoint[] {
  return map.points.filter((point): point is HeightMapPoint => point !== null)
}

/** The lowest and highest work Z of the measured points, mm. */
export function heightRange(map: HeightMap): { low: number; high: number } {
  const heights = measuredPoints(map).map((point) => point.work[2])
  return { low: Math.min(...heights), high: Math.max(...heights) }
}

const decimals = (unit: LengthUnit): number => unit === 'inch' ? 5 : 4
const unitName = (unit: LengthUnit): string => unit === 'inch' ? 'in' : 'mm'

/** What a saved map holds: work system, unit, rotation, date and the points measured. */
function frame(map: HeightMap, unit: LengthUnit, at: Date): Record<string, string | number> {
  return {
    system: wcsName(map.wcs), unit: unitName(unit), rotation_deg: map.rotation ?? 0, date: at.toISOString(),
    points: measuredPoints(map).length, grid_points: map.columns * map.rows,
    ...(map.partial === undefined ? {} : { partial: map.partial }),
  }
}

/**
 * One header comment, a column header and one line per measured point: work X, Y, Z, then
 * machine X, Y, Z, in `unit`. A partial map says so in the comment.
 */
export function heightMapCsv(map: HeightMap, unit: LengthUnit, at: Date): string {
  const name = unitName(unit)
  const value = (mm: number): string => toDisplay(mm, unit).toFixed(decimals(unit))
  const comment = '# CoffeeCNC height map ' + Object.entries(frame(map, unit, at)) // i18n-ignore: the CSV comment
    .map(([key, field]) => `${key}=${typeof field === 'string' && /[\s",]/.test(field) ? JSON.stringify(field) : String(field)}`).join(' ')
  const header = `work_x_${name},work_y_${name},work_z_${name},machine_x_${name},machine_y_${name},machine_z_${name}` // i18n-ignore: the CSV header
  return [comment, header, ...measuredPoints(map).map((point) => [...point.work, ...point.machine].map(value).join(','))].join('\n') + '\n'
}

/**
 * LinuxCNC's own probe file, as `(PROBEOPEN file)` writes it: one line per contact with
 * X Y Z A B C U V W in the work system, here in `unit`, the rotary and UVW axes 0. Nothing else,
 * so that readers of LinuxCNC's files take it: the frame goes in the .txt.json sidecar.
 */
export function heightMapProbeFile(map: HeightMap, unit: LengthUnit): string {
  const value = (mm: number): string => toDisplay(mm, unit).toFixed(6)
  return measuredPoints(map).map((point) => [...point.work.map(value), ...Array<string>(6).fill('0.000000')].join(' ')).join('\n') + '\n'
}

/** The probe file's sidecar: its work system, unit, rotation, date and whether it is partial. */
export function heightMapSidecar(map: HeightMap, unit: LengthUnit, at: Date): string {
  return JSON.stringify({ format: 'linuxcnc-probe', columns: map.columns, rows: map.rows, ...frame(map, unit, at) }, null, 2) + '\n'
}

/** heightmap-20261123-153000 (or …-partial): a file name for the program directory. */
export function heightMapName(at: Date, partial = false): string {
  const two = (value: number): string => String(value).padStart(2, '0')
  return `heightmap-${String(at.getFullYear())}${two(at.getMonth() + 1)}${two(at.getDate())}-` + // i18n-ignore: a file name
    `${two(at.getHours())}${two(at.getMinutes())}${two(at.getSeconds())}${partial ? '-partial' : ''}` // i18n-ignore: a file name
}
