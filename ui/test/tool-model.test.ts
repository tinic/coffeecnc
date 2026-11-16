import { describe, expect, it } from 'vitest'
import { Box3, LatheGeometry, Mesh, Vector2 } from 'three'
import type { Tool } from '../src/protocol'
import { decodeToolDefaults, factoryToolDefaults, resolveToolModel, toolDimensionError, toolFamilies, toolIdentity, toolProfiles,
  type ToolFamily, type ToolModelAssignment } from '../src/tool-model'
import { ToolMesh } from '../src/tool-mesh'

const tool: Tool = { number: 4, pocket: 7, diameter: 6, length: -143.52, comment: 'shop tool' }
function assignment(family: ToolFamily = 'end-mill'): ToolModelAssignment {
  return { identity: toolIdentity(tool), family, overrides: {} }
}

describe('parametric tool geometry', () => {
  it.each(toolFamilies.filter(({ id }) => id !== 'touch-probe'))('builds a finite $name envelope, tip at Z=0 and shank along +Z', ({ id }) => {
    const defaults = factoryToolDefaults()
    expect(toolDimensionError(id, defaults[id])).toBeNull()
    const model = resolveToolModel(tool, assignment(id), defaults)
    expect(model).not.toBeNull()
    if (model === null || !('cuttingLength' in model.dimensions)) throw new Error('Missing cutter')
    const profiles = toolProfiles(model)
    const bounds = Object.values(profiles).map((profile) => {
      const g = new LatheGeometry(profile.map(([r, z]) => new Vector2(r, z)), 64).rotateX(Math.PI / 2)
      g.computeBoundingBox()
      const box = g.boundingBox?.clone()
      g.dispose()
      return box
    })
    expect(bounds[0]?.min.z).toBeCloseTo(0)
    expect(bounds[0]?.max.z).toBeCloseTo(model.dimensions.cuttingLength)
    expect(bounds[1]?.min.z).toBeCloseTo(model.dimensions.cuttingLength)
    expect(bounds[1]?.max.z).toBeCloseTo(model.dimensions.overallLength)
    expect(bounds[0]?.max.x).toBeCloseTo(tool.diameter / 2)
  })
  it('builds a spherical probe ball from the table diameter, with its bottom at Z=0', () => {
    const model = resolveToolModel(tool, assignment('touch-probe'), factoryToolDefaults())
    if (model === null) throw new Error('Missing probe')
    const mesh = new ToolMesh()
    try {
      mesh.build(model)
      const [ball, stem, body, shank] = mesh.group.children
      expect(ball).toBeInstanceOf(Mesh)
      if (!ball || !stem || !body || !shank) throw new Error('Missing probe geometry')
      expect(new Box3().setFromObject(ball).min.z).toBeCloseTo(0)
      expect(new Box3().setFromObject(ball).max.z).toBeCloseTo(6)
      expect(new Box3().setFromObject(body).max.x).toBeCloseTo(14)
      expect(new Box3().setFromObject(body).min.z).toBeCloseTo(26)
      expect(new Box3().setFromObject(shank).max.z).toBeCloseTo(70)
    } finally { mesh.dispose() }
  })
  it('scales family lengths with diameter, never with the controller Z offset', () => {
    const model = resolveToolModel(tool, assignment(), factoryToolDefaults())
    if (model === null || !('cuttingLength' in model.dimensions)) throw new Error('Missing cutter')
    expect(model.dimensions.cuttingLength).toBeCloseTo(13.2)
    expect(model.dimensions.overallLength).toBeCloseTo(43.2)
    expect(resolveToolModel({ ...tool, length: 98765 }, assignment(), factoryToolDefaults())).toEqual(model)
    expect(resolveToolModel(tool, { ...assignment(), overrides: { shankDiameter: 8, stickout: 30 } }, factoryToolDefaults())?.dimensions)
      .toMatchObject({ diameter: 6, shankDiameter: 8, stickout: 30, cuttingLength: 13.2 })
  })
  it('keeps unknown tool families unassigned and rejects stale identities', () => {
    const defaults = factoryToolDefaults()
    expect(resolveToolModel(tool, undefined, defaults)).toBeNull()
    for (const changed of [{ ...tool, diameter: 8 }, { ...tool, comment: 'replacement' }, { ...tool, pocket: 8 }]) {
      expect(resolveToolModel(changed, assignment(), defaults)).toBeNull()
    }
    const unknown = { ...tool, diameter: 0 }
    const model = resolveToolModel(unknown, { ...assignment(), identity: toolIdentity(unknown) }, defaults)
    expect(model?.nominalDiameter).toBe(true)
    expect(model?.dimensions.diameter).toBe(10)
  })
  it('rejects impossible, nonfinite and unbounded dimensions before meshing', () => {
    const base = factoryToolDefaults()['end-mill']
    for (const patch of [{ diameter: Infinity }, { shankDiameter: NaN }, { diameter: 0 }, { overallLength: 10001 },
      { cuttingLength: 40 }, { stickout: 73 }, { overallLength: 22 }, { tipAngle: 0 }]) {
      expect(toolDimensionError('end-mill', { ...base, ...patch })).not.toBeNull()
      expect(() => toolProfiles({ family: 'end-mill', dimensions: { ...base, ...patch }, nominalDiameter: false, customized: true })).toThrow()
    }
    expect(toolDimensionError('ball-mill', { ...base, cuttingLength: 4 })).toContain('ball radius')
    expect(toolDimensionError('drill', { ...base, tipAngle: 10 })).toContain('pointed tip')
  })
  it('uses a spherical ball end and the included angle for pointed tips', () => {
    const defaults = factoryToolDefaults()
    for (const family of ['ball-mill', 'drill'] as const) {
      const model = resolveToolModel(tool, assignment(family), defaults)
      if (model === null) throw new Error('Missing model')
      const profile = toolProfiles(model).cutting
      if (family === 'ball-mill') {
        for (const [r, z] of profile.slice(0, 25)) expect(r * r + (z - 3) ** 2).toBeCloseTo(9)
      } else expect(profile[1]?.[1]).toBeCloseTo(3 / Math.tan(59 * Math.PI / 180))
    }
  })
  it('loads only validated known fields from versioned preferences', () => {
    const defaults = factoryToolDefaults()
    for (const text of [null, '{', 'null', '{}', '{"version":2,"defaults":{}}']) expect(decodeToolDefaults(text)).toEqual(defaults)
    const altered = { ...defaults, drill: { ...defaults.drill, stickout: -1 },
      'end-mill': { ...defaults['end-mill'], stickout: 40, surprise: 'ignored' } }
    const result = decodeToolDefaults(JSON.stringify({ version: 1, defaults: altered }))
    expect(result.drill).toEqual(defaults.drill)
    expect(result['end-mill']).toEqual({ ...defaults['end-mill'], stickout: 40 })
  })
})
