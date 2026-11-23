import type { Offsets, Vec3 } from './protocol'

export const maxStepBytes = 25 * 1024 * 1024
export const maxModelTriangles = 500_000
export const maxModelVertices = 800_000
export const importTimeoutMs = 30_000

export interface ModelMesh {
  name: string
  positions: Float32Array<ArrayBuffer>
  normals: Float32Array<ArrayBuffer> | null
  indices: Uint32Array<ArrayBuffer>
  color: Vec3 | null
}
export interface ModelGeometry {
  meshes: ModelMesh[]
  bounds: { min: Vec3; max: Vec3 }
  triangles: number
}
export interface Placement { translation: Vec3; rotation: Vec3 }
export type ModelFrame = Omit<Offsets, 'tool'>
export interface ModelBinding { file: string; revision: number; frame: ModelFrame }
export type ImportReply = { ok: true; geometry: ModelGeometry } | { ok: false; error: string }

function object(value: unknown): Record<string, unknown> {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) throw new Error('Invalid STEP mesh data')
  return value as Record<string, unknown>
}

function numbers(value: unknown, limit: number): number[] {
  if (!Array.isArray(value) || value.length > limit || !value.every((n: unknown) => typeof n === 'number' && Number.isFinite(n))) {
    throw new Error('Invalid or oversized STEP mesh data')
  }
  return value as number[]
}

/** Validate the imported geometry before moving it out of the worker. */
export function normalizeModel(value: unknown): ModelGeometry {
  const result = object(value)
  if (result['success'] !== true) throw new Error('Could not read this STEP file')
  const rawMeshes: unknown = result['meshes']
  if (!Array.isArray(rawMeshes) || rawMeshes.length === 0 || rawMeshes.length > 4096) {
    throw new Error('STEP file contains no supported display geometry, or too many parts')
  }
  const min: Vec3 = [Infinity, Infinity, Infinity]
  const max: Vec3 = [-Infinity, -Infinity, -Infinity]
  let triangles = 0
  let vertices = 0
  const meshes = rawMeshes.map((raw: unknown): ModelMesh => {
    const mesh = object(raw)
    const attributes = object(mesh['attributes'])
    const positions = numbers(object(attributes['position'])['array'], maxModelVertices * 3)
    const indices = numbers(object(mesh['index'])['array'], maxModelTriangles * 3)
    vertices += positions.length / 3
    triangles += indices.length / 3
    if (!positions.length || !indices.length || positions.length % 3 || indices.length % 3 ||
        vertices > maxModelVertices || triangles > maxModelTriangles) throw new Error('STEP mesh exceeds the display limit')
    positions.forEach((n, index) => {
      if (Math.abs(n) > 1e7) throw new Error('STEP coordinates exceed the display range')
      const axis = (index % 3) as 0 | 1 | 2
      min[axis] = Math.min(min[axis], n)
      max[axis] = Math.max(max[axis], n)
    })
    if (!indices.every((n) => Number.isInteger(n) && n >= 0 && n < positions.length / 3)) {
      throw new Error('STEP mesh contains an invalid triangle')
    }
    const normals = attributes['normal'] === undefined ? null : numbers(object(attributes['normal'])['array'], positions.length)
    if (normals !== null && normals.length !== positions.length) throw new Error('STEP mesh normals are incomplete')
    const color = mesh['color'] === undefined ? null : numbers(mesh['color'], 3)
    if (color !== null && (color.length !== 3 || color.some((n) => n < 0 || n > 1))) throw new Error('Invalid STEP color')
    return {
      name: typeof mesh['name'] === 'string' ? mesh['name'] : '',
      positions: new Float32Array(positions), normals: normals === null ? null : new Float32Array(normals),
      indices: new Uint32Array(indices), color: color as Vec3 | null,
    }
  })
  return { meshes, bounds: { min, max }, triangles }
}

export function frameOf(offsets: Offsets): ModelFrame {
  return { wcs: offsets.wcs, g5x: [...offsets.g5x], g92: [...offsets.g92], rotation_xy: offsets.rotation_xy }
}

export function sameFrame(a: ModelFrame, b: ModelFrame): boolean {
  return a.wcs === b.wcs && a.rotation_xy === b.rotation_xy &&
    a.g5x.every((n, i) => n === b.g5x[i]) && a.g92.every((n, i) => n === b.g92[i])
}

/** Intrinsic placement is explicitly Rx, then Ry, then Rz, then translation in work mm.
 * Work to machine-tip coordinates is G92, XY rotation, G5x. Never add the tool offset. */
export function modelToMachine(point: Vec3, placement: Placement, frame: ModelFrame): Vec3 {
  const [rx, ry, rz] = placement.rotation.map((n) => n * Math.PI / 180) as Vec3
  const [x, y, z] = point
  const y1 = y * Math.cos(rx) - z * Math.sin(rx)
  const z1 = y * Math.sin(rx) + z * Math.cos(rx)
  const x2 = x * Math.cos(ry) + z1 * Math.sin(ry)
  const z2 = -x * Math.sin(ry) + z1 * Math.cos(ry)
  const px = x2 * Math.cos(rz) - y1 * Math.sin(rz) + placement.translation[0] + frame.g92[0]
  const py = x2 * Math.sin(rz) + y1 * Math.cos(rz) + placement.translation[1] + frame.g92[1]
  const a = frame.rotation_xy * Math.PI / 180
  return [px * Math.cos(a) - py * Math.sin(a) + frame.g5x[0],
    px * Math.sin(a) + py * Math.cos(a) + frame.g5x[1], z2 + placement.translation[2] + frame.g92[2] + frame.g5x[2]]
}
