import { describe, expect, it } from 'vitest'
import {
  heightGrid, heightMapAnswerMs, heightMapCsv, heightMapName, heightMapOrder, heightMapPointAt, heightMapPointSeconds,
  heightMapProbeFile, heightMapSeconds, heightMapSidecar, heightRange, maxHeightMapSeconds, probeAnswerMs,
} from '../src/height-map'
import type { HeightMap } from '../src/protocol'

// The same grid as the controller's (backend/tests/core_tests.cpp height_map_grid).
describe('height map', () => {
  it('lays the grid from corner to corner, at most a step apart', () => {
    expect(heightGrid([0, 0, 20, 16], 10)).toEqual({ columns: 3, rows: 3 })
    expect(heightGrid([-5, 2, 12, 2], 4)).toEqual({ columns: 6, rows: 1 })
    expect(heightGrid([20, 16, 0, 0], 10)).toEqual({ columns: 3, rows: 3 })
    expect(heightGrid([0, 0, 0, 0], 10)).toBeNull()
    expect(heightGrid([0, 0, 100, 100], 5)).toBeNull()
    expect(heightGrid([0, 0, 20, 16], 0.25)).toBeNull()
  })

  const at = new Date(Date.UTC(2026, 10, 23, 23, 30, 5))
  const map: HeightMap = { wcs: 2, columns: 2, rows: 1, rotation: 7.5, points: [
    { work: [0, 0, 0.0127], machine: [5, -7, 12.0127] },
    { work: [25.4, 0, -0.0254], machine: [30.4, -7, 11.9746] },
  ] }

  it('exports CSV and the LinuxCNC probe file in the display unit', () => {
    expect(heightMapCsv(map, 'mm', at)).toBe(
      '# CoffeeCNC height map system=G55 unit=mm rotation_deg=7.5 date=2026-11-23T23:30:05.000Z points=2 grid_points=2\n' +
      'work_x_mm,work_y_mm,work_z_mm,machine_x_mm,machine_y_mm,machine_z_mm\n' +
      '0.0000,0.0000,0.0127,5.0000,-7.0000,12.0127\n25.4000,0.0000,-0.0254,30.4000,-7.0000,11.9746\n')
    expect(heightMapCsv(map, 'inch', at).split('\n').slice(0, 3)).toEqual([
      '# CoffeeCNC height map system=G55 unit=in rotation_deg=7.5 date=2026-11-23T23:30:05.000Z points=2 grid_points=2',
      'work_x_in,work_y_in,work_z_in,machine_x_in,machine_y_in,machine_z_in', '0.00000,0.00000,0.00050,0.19685,-0.27559,0.47294'])
    expect(heightMapCsv(map, 'inch', at).split('\n')[3]).toBe('1.00000,0.00000,-0.00100,1.19685,-0.27559,0.47144')
    // X Y Z A B C U V W, as (PROBEOPEN) writes each contact.
    expect(heightMapProbeFile(map, 'inch')).toBe(
      '0.000000 0.000000 0.000500 0.000000 0.000000 0.000000 0.000000 0.000000 0.000000\n' +
      '1.000000 0.000000 -0.001000 0.000000 0.000000 0.000000 0.000000 0.000000 0.000000\n')
    expect(heightRange(map)).toEqual({ low: -0.0254, high: 0.0127 })
    expect(heightMapName(new Date(2026, 10, 23, 15, 30, 5))).toBe('heightmap-20261123-153005')
    // The probe file stays pure; its frame is in the sidecar.
    expect(JSON.parse(heightMapSidecar(map, 'inch', at))).toEqual({
      format: 'linuxcnc-probe', columns: 2, rows: 1, system: 'G55', unit: 'in', rotation_deg: 7.5,
      date: '2026-11-23T23:30:05.000Z', points: 2, grid_points: 2,
    })
  })

  it('exports only the measured points of a partial map, labelled partial', () => {
    const partial: HeightMap = { ...map, points: [map.points[0] ?? null, null], partial: 'The height map was stopped.' }
    const csv = heightMapCsv(partial, 'mm', at).split('\n')
    expect(csv[0]).toBe('# CoffeeCNC height map system=G55 unit=mm rotation_deg=7.5 date=2026-11-23T23:30:05.000Z ' +
      'points=1 grid_points=2 partial="The height map was stopped."')
    expect(csv.slice(2)).toEqual(['0.0000,0.0000,0.0127,5.0000,-7.0000,12.0127', ''])
    expect(heightMapProbeFile(partial, 'mm').split('\n')).toHaveLength(2)
    expect(JSON.parse(heightMapSidecar(partial, 'mm', at))).toMatchObject({ points: 1, grid_points: 2, partial: 'The height map was stopped.' })
    expect(heightRange(partial)).toEqual({ low: 0.0127, high: 0.0127 })
    expect(heightMapName(new Date(2026, 10, 23, 15, 30, 5), true)).toBe('heightmap-20261123-153005-partial')
  })

  // The same worst case as the controller's (backend/tests/core_tests.cpp height_map_grid).
  const limits = { reference_tool: 98, travel_mm: 15, feed_mm_min: 240, xy_tolerance_mm: 0.5, latch_feed_mm_min: 60,
    latch_backoff_mm: 1, material_travel_mm: 20 }
  it('bounds the time of a map as the controller does', () => {
    expect(heightMapPointSeconds(limits)).toBeCloseTo(31.875, 9)
    expect(heightMapSeconds(limits, 150, 5)).toBeCloseTo(150 * 31.875 + 0.625, 9)
    expect(heightMapSeconds({ ...limits, latch_feed_mm_min: 5 }, 150, 0)).toBeGreaterThan(maxHeightMapSeconds)
    expect(heightMapPointSeconds({ reference_tool: 98, travel_mm: 15, feed_mm_min: 240, xy_tolerance_mm: 0.5 })).toBe(Infinity)
  })

  it('waits for a 150-point map as long as its worst case, not the 1600 s of one cycle', () => {
    // 15 × 10 points at 10 mm, the clearance 5 mm under the start: 4781.25 s, 0.625 s and 120 s.
    const grid = { x_min: 0, y_min: 0, x_max: 140, y_max: 90, step: 10, clearance: 3 }
    expect(Math.abs(heightMapAnswerMs(limits, grid, 20, 12) - (4_781_875 + 120_000))).toBeLessThanOrEqual(1)
    expect(heightMapAnswerMs(limits, { ...grid, x_max: 10, y_max: 0 }, 20, 12)).toBe(probeAnswerMs)
    expect(heightMapAnswerMs(undefined, grid, 20, 12)).toBe(probeAnswerMs)
  })

  it('names the point of the probing order the probe is over', () => {
    expect(heightMapOrder(3, 2)).toEqual([0, 1, 2, 5, 4, 3])
    const grid = { x_min: -5, y_min: -4, x_max: 15, y_max: 4, step: 10, clearance: 3 }
    expect(heightMapPointAt(grid, -5, -4)).toBe(1)
    expect(heightMapPointAt(grid, 15.2, -4.1)).toBe(3)
    expect(heightMapPointAt(grid, 15, 4)).toBe(4)
    expect(heightMapPointAt(grid, -5, 4)).toBe(6)
    expect(heightMapPointAt(grid, 0, -4)).toBeNull()
  })
})
