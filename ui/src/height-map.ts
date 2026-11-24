// The last Z height map (#247): shown in the 3D view, saved as CSV and as a LinuxCNC probe file.
import { shallowRef } from 'vue'
import type { HeightMap, LengthUnit } from './protocol'
import { toDisplay } from './units'

/** The grid a height map probes, in work millimetres: corners, step and the clearance Z. */
export interface HeightMapGrid { x_min: number; y_min: number; x_max: number; y_max: number; step: number; clearance: number }

/** The map the 3D view draws, until another one replaces it or the operator hides it. */
export const heightMap = shallowRef<HeightMap | null>(null)

/** At most this many points, as natively (fixed_setter.hpp max_height_map_points). */
export const maxHeightMapPoints = 150

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

/** The lowest and highest work Z of the map, mm. */
export function heightRange(map: HeightMap): { low: number; high: number } {
  const heights = map.points.map((point) => point.work[2])
  return { low: Math.min(...heights), high: Math.max(...heights) }
}

const decimals = (unit: LengthUnit): number => unit === 'inch' ? 5 : 4

/** One line per point: work X, Y, Z, then machine X, Y, Z, in `unit`. */
export function heightMapCsv(map: HeightMap, unit: LengthUnit): string {
  const name = unit === 'inch' ? 'in' : 'mm'
  const value = (mm: number): string => toDisplay(mm, unit).toFixed(decimals(unit))
  const header = `work_x_${name},work_y_${name},work_z_${name},machine_x_${name},machine_y_${name},machine_z_${name}` // i18n-ignore: the CSV header
  return [header, ...map.points.map((point) => [...point.work, ...point.machine].map(value).join(','))].join('\n') + '\n'
}

/**
 * LinuxCNC's own probe file, as `(PROBEOPEN file)` writes it: one line per contact with
 * X Y Z A B C U V W in the work system, here in `unit`, the rotary and UVW axes 0.
 */
export function heightMapProbeFile(map: HeightMap, unit: LengthUnit): string {
  const value = (mm: number): string => toDisplay(mm, unit).toFixed(6)
  return map.points.map((point) => [...point.work.map(value), ...Array<string>(6).fill('0.000000')].join(' ')).join('\n') + '\n'
}

/** heightmap-20261123-153000: a file name for the program directory. */
export function heightMapName(at: Date): string {
  const two = (value: number): string => String(value).padStart(2, '0')
  return `heightmap-${String(at.getFullYear())}${two(at.getMonth() + 1)}${two(at.getDate())}-` + // i18n-ignore: a file name
    `${two(at.getHours())}${two(at.getMinutes())}${two(at.getSeconds())}`
}
