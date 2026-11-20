import { describe, expect, it } from 'vitest'
import { Box3, BufferAttribute, BufferGeometry, LatheGeometry, Mesh, Vector2 } from 'three'
import type { Tool } from '../src/protocol'
import { decodeToolDefaults, factoryToolDefaults, pointedTool, resolveToolModel, toolDimensionError, toolFamilies, toolIdentity, toolProfiles,
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
  it.each([
    ['end-mill', 3], ['ball-mill', 2], ['drill', 2], ['spot-drill', 2], ['chamfer', 4],
  ] as const)('renders %s with %s helical cutting edges inside its envelope', (family, flutes) => {
    const model = resolveToolModel(tool, assignment(family), factoryToolDefaults())
    if (model === null || !('cuttingLength' in model.dimensions)) throw new Error('Missing cutter')
    const mesh = new ToolMesh()
    try {
      mesh.build(model)
      const cutter = mesh.group.getObjectByName('cutter')
      if (!(cutter instanceof Mesh)) throw new Error('Missing cutter mesh')
      const bounds = new Box3().setFromObject(cutter)
      expect(bounds.min.z).toBeCloseTo(0)
      expect(bounds.max.z).toBeCloseTo(model.dimensions.cuttingLength)
      const geometry: unknown = cutter.geometry
      if (!(geometry instanceof BufferGeometry)) throw new Error('Missing cutter geometry')
      const positions: unknown = geometry.getAttribute('position')
      if (!(positions instanceof BufferAttribute)) throw new Error('Missing cutter positions')
      const radii = Array.from({ length: positions.count }, (_, i) =>
        Math.hypot(positions.getX(i), positions.getY(i)))
      expect(Math.max(...radii)).toBeCloseTo(tool.diameter / 2)
      const radius = tool.diameter / 2
      const length = model.dimensions.cuttingLength
      const lipZ = positions.getZ(0)
      const lipRadius = Math.hypot(positions.getX(0), positions.getY(0))
      if (family === 'ball-mill') expect(lipRadius ** 2 + (lipZ - radius) ** 2).toBeCloseTo(radius ** 2)
      if (pointedTool(family)) expect(Math.atan2(lipRadius, lipZ) * 360 / Math.PI).toBeCloseTo(model.dimensions.tipAngle)
      const angleAt = (fraction: number): number => {
        const start = radii.findIndex((_, i) => positions.getZ(i) >= fraction * length)
        const z = positions.getZ(start)
        const ring = radii.map((r, i) => ({ r, i })).filter(({ i }) => positions.getZ(i) === z)
        expect(Math.min(...ring.map(({ r }) => r))).toBeLessThan(radius * 0.75)
        const edges = ring.filter(({ r }) => Math.abs(r - radius) < radius * 0.0001)
        expect(edges).toHaveLength(flutes)
        const [edge] = edges
        if (!edge) throw new Error('Missing cutting edge')
        return Math.atan2(positions.getY(edge.i), positions.getX(edge.i))
      }
      expect(Math.abs(angleAt(0.74) - angleAt(0.52))).toBeGreaterThan(0.035)
    } finally { mesh.dispose() }
  })
  it('places the face mill inserts at the tip of the nominal cutter envelope', () => {
    const dimensions = factoryToolDefaults()['face-mill']
    const mesh = new ToolMesh()
    try {
      mesh.build({ family: 'face-mill', dimensions, nominalDiameter: false, customized: false })
      const head = mesh.group.getObjectByName('cutter')
      if (!head) throw new Error('Missing face mill')
      const inserts = head.children.filter(({ name }) => name === 'insert')
      const screws = head.children.filter(({ name }) => name === 'insert screw')
      expect(inserts).toHaveLength(4)
      expect(screws).toHaveLength(4)
      for (const insert of inserts) {
        expect(new Box3().setFromObject(insert).min.z).toBeCloseTo(0)
      }
      const bounds = new Box3().setFromObject(head)
      expect(bounds.max.z).toBeCloseTo(dimensions.cuttingLength)
      expect(bounds.max.x).toBeLessThanOrEqual(dimensions.diameter / 2)
    } finally { mesh.dispose() }
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
