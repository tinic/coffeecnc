import { describe, expect, it } from 'vitest'
import { OrthographicCamera, Vector3 } from 'three'
import { syncModelCamera } from '../src/model-camera'
import { orbit, pan, project, views, zoom, type Camera } from '../src/pathview'
import { modelBlock } from '../src/step-block'
import { frameOf, modelToMachine, normalizeModel, sameFrame, type ModelBinding, type Placement } from '../src/step-model'
import { SimMachine } from '../src/sim/machine'
import type { Offsets, Preview, Vec3 } from '../src/protocol'

const offsets: Offsets = { wcs: 1, g5x: [100, 200, -40], g92: [10, 20, 3], rotation_xy: 90, tool: [4, 5, 80] }
const identity: Placement = { translation: [0, 0, 0], rotation: [0, 0, 0] }
const rawMesh = { name: 'part', color: [0.5, 0.6, 0.7], attributes: { position: { array: [0, 0, 0, 10, 0, 0, 0, 10, 0] } }, index: { array: [0, 1, 2] } }

describe('STEP display geometry', () => {
  it('produces bounded transferable buffers from importer output', () => {
    const model = normalizeModel({ success: true, meshes: [rawMesh] })
    expect(model.triangles).toBe(1)
    expect(model.bounds).toEqual({ min: [0, 0, 0], max: [10, 10, 0] })
    expect(model.meshes[0]?.positions).toBeInstanceOf(Float32Array)
    expect(model.meshes[0]?.indices).toBeInstanceOf(Uint32Array)
    expect(model.meshes[0]?.normals).toBeNull()
  })
  it('rejects failed, empty and corrupt imports', () => {
    for (const value of [null, { success: false }, { success: true, meshes: [] },
      { success: true, meshes: [{ ...rawMesh, index: { array: [0, 1, 3] } }] },
      { success: true, meshes: [{ ...rawMesh, attributes: { position: { array: [NaN, 0, 0] } } }] },
      { success: true, meshes: [{ ...rawMesh, color: [1, 2, 3] }] }]) {
      expect(() => normalizeModel(value)).toThrow()
    }
  })
})

describe('model registration', () => {
  it('applies G92 before XY rotation and G5x, without tool length', () => {
    const p = modelToMachine([1, 2, 3], identity, frameOf(offsets))
    expect(p[0]).toBeCloseTo(78)
    expect(p[1]).toBeCloseTo(211)
    expect(p[2]).toBeCloseTo(-34)
  })
  it('rotates X then Y then Z about CAD origin before work translation', () => {
    const placement: Placement = { translation: [5, 6, 7], rotation: [90, 90, 90] }
    const origin = { wcs: 1, g5x: [0, 0, 0] as Vec3, g92: [0, 0, 0] as Vec3, rotation_xy: 0 }
    const p = modelToMachine([1, 2, 3], placement, origin)
    expect(p[0]).toBeCloseTo(8)
    expect(p[1]).toBeCloseTo(8)
    expect(p[2]).toBeCloseTo(6)
  })
  it('captures an independent frame and ignores tool offsets when comparing', () => {
    const frame = frameOf(offsets)
    const changed = { ...offsets, tool: [0, 0, 999] as Vec3 }
    expect(sameFrame(frame, changed)).toBe(true)
    changed.g5x = [101, 200, -40]
    expect(sameFrame(frame, changed)).toBe(false)
    expect(frame.g5x[0]).toBe(100)
  })
  it('hides models when the program, work frame, preview or connection changes', () => {
    const status = new SimMachine({ emit: (): void => undefined }).status()
    status.program.file = 'part.ngc'
    const binding: ModelBinding = { file: 'part.ngc', revision: status.program.revision, frame: frameOf(status.offsets) }
    const preview: Preview = { type: 'preview', state: 'ready', context: status.preview_context, error: null, bounds: null, dwells: [], segments: [] }
    expect(modelBlock(binding, status, preview, true)).toBe('')
    expect(modelBlock(binding, status, preview, false)).toContain('unavailable')
    expect(modelBlock(binding, status, { ...preview, state: 'pending' }, true)).toContain('waiting')
    expect(modelBlock(binding, status, { ...preview, context: preview.context + 1 }, true)).toContain('waiting')
    expect(modelBlock(binding, { ...status, program: { ...status.program, revision: status.program.revision + 1 } }, preview, true)).toContain('program changed')
    expect(modelBlock(binding, { ...status, offsets: { ...status.offsets, wcs: 2 } }, preview, true)).toContain('work origin changed')
    expect(modelBlock(binding, { ...status, offsets: { ...status.offsets, g92: [1, 0, 0] } }, preview, true)).toContain('work origin changed')
  })
})

describe('model and toolpath share a camera', () => {
  it('matches projected pixels after orbit, pan, zoom and named views', () => {
    const width = 833; const height = 477
    const points: Vec3[] = [[12, -43, 18], [120, 250, -80], [-500, 22, 1]]
    const base: Camera = { ...views.iso, scale: 2.7, centre: [111, 25, -40] }
    const cameras = [base, orbit(base, 113, 29), pan(base, 72, -41), zoom(base, 1.8, [100, 160], width, height),
      { ...base, ...views.top }, { ...base, ...views.front }]
    for (const camera of cameras) {
      const target = new OrthographicCamera()
      syncModelCamera(target, camera, width, height, 3000)
      for (const point of points) {
        const projected = new Vector3(...point).project(target)
        const expected = project(camera, point, width, height)
        expect((projected.x + 1) * width / 2).toBeCloseTo(expected[0], 7)
        expect((1 - projected.y) * height / 2).toBeCloseTo(expected[1], 7)
        expect(Math.abs(projected.z)).toBeLessThan(1)
      }
    }
  })
})
