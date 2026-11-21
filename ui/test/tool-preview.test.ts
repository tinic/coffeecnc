import { describe, expect, it } from 'vitest'
import { Box3 } from 'three'
import type { Status, Tool } from '../src/protocol'
import { SimMachine } from '../src/sim/machine'
import { factoryToolDefaults, toolIdentity, type ToolModelAssignment } from '../src/tool-model'
import { ToolMesh } from '../src/tool-mesh'
import { loadedToolPreview, reportedToolTip, toolPreviewBounds } from '../src/tool-preview'

const tool: Tool = { number: 4, pocket: 7, diameter: 6, length: -143.52, comment: 'shop tool' }
const assignments = new Map<number, ToolModelAssignment>([[4, { identity: toolIdentity(tool), family: 'end-mill', overrides: {} }]])
function status(): Status {
  const s = new SimMachine({ emit: (): void => undefined }).status()
  s.link = 'live'
  s.axes.forEach((axis, i) => { axis.homed = true; axis.machine = [101, 202, 303][i] ?? 0 })
  s.offsets.tool = [1, 2, 30]
  s.offsets.g5x = [500, 600, 700]; s.offsets.g92 = [8, 9, 10]; s.offsets.rotation_xy = 90
  s.tool = { in_spindle: 4, prepared: 9 }
  return s
}

describe('loaded tool preview', () => {
  it('uses the loaded tool, applies the active offset once and keeps body along machine +Z', () => {
    const preview = loadedToolPreview(status(), true, [tool], assignments, factoryToolDefaults())
    if (preview === null) throw new Error('Missing loaded tool')
    expect(preview.number).toBe(4)
    expect(preview.tip).toEqual([100, 200, 273])
    const mesh = new ToolMesh()
    mesh.build(preview.model)
    mesh.group.position.set(...preview.tip)
    const bounds = new Box3().setFromObject(mesh.group)
    expect(bounds.min.z).toBeCloseTo(273)
    expect(bounds.max.z).toBeCloseTo(273 + 43.2)
    expect(bounds.getCenter(bounds.min.clone()).x).toBeCloseTo(100)
    const fitBounds = toolPreviewBounds(preview)
    expect(fitBounds.min[0]).toBeLessThanOrEqual(bounds.min.x)
    expect(fitBounds.max[0]).toBeGreaterThanOrEqual(bounds.max.x)
    mesh.dispose()
    // Table Z and work offsets are neither physical dimensions nor a second placement offset.
    expect(loadedToolPreview(status(), true, [{ ...tool, length: 999 }], assignments, factoryToolDefaults())).toEqual(preview)
    const withoutG43 = status(); withoutG43.offsets.tool = [0, 0, 0]
    expect(reportedToolTip(withoutG43)).toEqual([101, 202, 303])
  })

  it('hides stale, unhomed, changing, missing, unassigned and invalid tool bodies', () => {
    const cases: Status[] = [
      { ...status(), link: 'stale' },
      { ...status(), axes: status().axes.map((axis) => ({ ...axis, homed: false })) },
      { ...status(), wait: { kind: 'tool_change', tool: 9 } },
      { ...status(), tool: { in_spindle: 0, prepared: 9 } },
      { ...status(), tool: { in_spindle: 9, prepared: 4 } },
      { ...status(), axes: status().axes.map((axis) => ({ ...axis, machine: NaN })) },
    ]
    for (const s of cases) expect(loadedToolPreview(s, true, [tool], assignments, factoryToolDefaults())).toBeNull()
    expect(loadedToolPreview(status(), false, [tool], assignments, factoryToolDefaults())).toBeNull()
    expect(loadedToolPreview(null, true, [tool], assignments, factoryToolDefaults())).toBeNull()
    expect(loadedToolPreview(status(), true, [], assignments, factoryToolDefaults())).toBeNull()
    expect(loadedToolPreview(status(), true, [tool], new Map(), factoryToolDefaults())).toBeNull()
    expect(loadedToolPreview(status(), true, [{ ...tool, diameter: 8 }], assignments, factoryToolDefaults())).toBeNull()
    const invalid = factoryToolDefaults(); invalid['end-mill'].stickout = 1
    expect(loadedToolPreview(status(), true, [tool], assignments, invalid)).toBeNull()
  })

  it('fits the chosen holder at the stick-out above the same tool tip', () => {
    const equipped = new Map<number, ToolModelAssignment>([[4, {
      identity: toolIdentity(tool), family: 'end-mill', overrides: { stickout: 25 }, holder: 'tts-er32',
    }]])
    const preview = loadedToolPreview(status(), true, [tool], equipped, factoryToolDefaults())
    if (preview === null) throw new Error('Missing loaded tool')
    expect(preview.tip).toEqual([100, 200, 273])
    const mesh = new ToolMesh()
    try {
      mesh.build(preview.model)
      const holder = mesh.group.getObjectByName('holder')
      if (!holder) throw new Error('Missing holder mesh')
      const body = new Box3().setFromObject(holder)
      expect(body.min.z).toBeCloseTo(25)
      expect(body.max.x).toBeCloseTo(25)
      expect(toolPreviewBounds(preview).max[2]).toBeCloseTo(preview.tip[2] + body.max.z)
      mesh.build({ ...preview.model, holder: 'r8' })
      expect(mesh.group.getObjectByName('holder')).toBeUndefined()
      expect(new Box3().setFromObject(mesh.group).max.z).toBeCloseTo(25)
    } finally { mesh.dispose() }
  })

  it('matches axes by name and rejects missing, homing or nonfinite coordinates', () => {
    const s = status(); s.axes.reverse()
    expect(reportedToolTip(s)).toEqual([100, 200, 273])
    s.axes.forEach((axis) => { axis.homed = false })
    // Not homed: unknown only where homing is required (the G0759 runs without it).
    expect(reportedToolTip(s)).toBeNull()
    expect(reportedToolTip(s, false)).toEqual([100, 200, 273])
    s.axes.forEach((axis) => { axis.homed = true; axis.homing = true })
    expect(reportedToolTip(s, false)).toBeNull()
    expect(reportedToolTip(s)).toBeNull()
    s.axes.pop()
    expect(reportedToolTip(s)).toBeNull()
    expect(reportedToolTip({ ...status(), offsets: { ...status().offsets, tool: [0, Infinity, 0] } })).toBeNull()
  })
})
