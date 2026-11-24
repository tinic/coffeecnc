import { describe, expect, it } from 'vitest'
import { heightGrid, heightMapCsv, heightMapName, heightMapProbeFile, heightRange } from '../src/height-map'
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

  const map: HeightMap = { wcs: 2, columns: 2, rows: 1, points: [
    { work: [0, 0, 0.0127], machine: [5, -7, 12.0127] },
    { work: [25.4, 0, -0.0254], machine: [30.4, -7, 11.9746] },
  ] }

  it('exports CSV and the LinuxCNC probe file in the display unit', () => {
    expect(heightMapCsv(map, 'mm')).toBe(
      'work_x_mm,work_y_mm,work_z_mm,machine_x_mm,machine_y_mm,machine_z_mm\n' +
      '0.0000,0.0000,0.0127,5.0000,-7.0000,12.0127\n25.4000,0.0000,-0.0254,30.4000,-7.0000,11.9746\n')
    expect(heightMapCsv(map, 'inch').split('\n')[2]).toBe('1.00000,0.00000,-0.00100,1.19685,-0.27559,0.47144')
    // X Y Z A B C U V W, as (PROBEOPEN) writes each contact.
    expect(heightMapProbeFile(map, 'inch')).toBe(
      '0.000000 0.000000 0.000500 0.000000 0.000000 0.000000 0.000000 0.000000 0.000000\n' +
      '1.000000 0.000000 -0.001000 0.000000 0.000000 0.000000 0.000000 0.000000 0.000000\n')
    expect(heightRange(map)).toEqual({ low: -0.0254, high: 0.0127 })
    expect(heightMapName(new Date(2026, 10, 23, 15, 30, 5))).toBe('heightmap-20261123-153005')
  })
})
