import { BufferGeometry, ExtrudeGeometry, Float32BufferAttribute, Matrix4, Path, Shape } from 'three'
import type { ToolFamily } from './tool-model'

type Contour = readonly (readonly [fraction: number, radiusRatio: number])[]
type Profile = readonly (readonly [radius: number, z: number])[]
interface Ring { z: number; radius: number; phase: number; depth: number }

// One tooth or pocket, expressed over its share of the circumference.
const millTooth: Contour = [
  [0, 1], [0.07, 0.99], [0.15, 0.95], [0.24, 0.83], [0.34, 0.66],
  [0.44, 0.59], [0.54, 0.57], [0.64, 0.58], [0.74, 0.62], [0.82, 0.69],
  [0.89, 0.78], [0.94, 0.88], [0.98, 0.96],
]
const drillTooth: Contour = [
  [0, 1], [0.04, 0.999], [0.09, 0.96], [0.22, 0.94], [0.36, 0.92],
  [0.47, 0.87], [0.54, 0.7], [0.61, 0.53], [0.68, 0.47], [0.75, 0.48],
  [0.82, 0.53], [0.89, 0.63], [0.94, 0.75], [0.98, 0.92],
]

const flutedFamilies = {
  'end-mill': { count: 3, helix: 30, contour: millTooth },
  'ball-mill': { count: 2, helix: 30, contour: millTooth },
  drill: { count: 2, helix: 30, contour: drillTooth },
  'spot-drill': { count: 2, helix: 20, contour: drillTooth },
  chamfer: { count: 4, helix: 10, contour: millTooth },
} as const

export function flutedCutterGeometry(family: ToolFamily, profile: Profile, diameter: number, length: number): BufferGeometry | null {
  if (!isFluted(family)) return null
  const { count, helix, contour } = flutedFamilies[family]
  // Cap the turns on extreme dimensions so the preview still resolves the flutes.
  const twist = Math.min(Math.tan(helix * Math.PI / 180) * length / (diameter / 2), 6 * Math.PI)
  const rings: Ring[] = []
  const addRing = (radius: number, z: number): void => {
    const t = z / length
    const runout = Math.min(1, (1 - t) / 0.18)
    rings.push({ radius, z, phase: twist * t, depth: runout * runout * (3 - 2 * runout) })
  }

  // Skip the end cap centres. Interpolate along the original cone, ball or cylinder
  // and keep their profile junctions as exact cutting-edge rings.
  const outline = profile.slice(profile[1]?.[1] === 0 ? 1 : 0, -1)
  const [first] = outline
  if (!first) return null
  addRing(...first)
  let [startR, startZ] = first
  for (const [endR, endZ] of outline.slice(1)) {
    const span = endZ - startZ
    const steps = span > Number.EPSILON * length ? Math.ceil(64 * span / length) : 0
    for (let j = 1; j <= steps; j += 1) {
      const t = j / steps
      addRing(startR + (endR - startR) * t, startZ + (endZ - startZ) * t)
    }
    startR = endR
    startZ = endZ
  }
  return sweptGeometry(rings, contour, count)
}

function isFluted(family: ToolFamily): family is keyof typeof flutedFamilies {
  return Object.hasOwn(flutedFamilies, family)
}

const insertPocket: Contour = [
  [0, 0.82], [0.12, 0.83], [0.19, 0.85], [0.23, 0.92], [0.3, 1], [0.44, 1],
  [0.56, 1], [0.68, 1], [0.76, 0.97], [0.84, 0.9], [0.92, 0.83],
]

export function faceMillGeometry(diameter: number, length: number): {
  body: BufferGeometry; inserts: BufferGeometry[]; screws: BufferGeometry[]
} {
  const radius = diameter / 2
  const insertCount = 4
  const rings: Ring[] = [
    { radius: radius * 0.85, z: length * 0.12, depth: 1, phase: 0 },
    { radius: radius * 0.98, z: length * 0.3, depth: 1, phase: 0 },
    { radius: radius * 0.98, z: length * 0.62, depth: 1, phase: 0 },
    { radius: radius * 0.98, z: length * 0.83, depth: 0.22, phase: 0 },
    { radius: radius * 0.94, z: length * 0.95, depth: 0, phase: 0 },
    { radius: radius * 0.89, z: length, depth: 0, phase: 0 },
  ]
  const body = sweptGeometry(rings, insertPocket, insertCount)

  const span = 0.18
  const outside = radius * Math.sqrt(1 - span * span)
  const shape = new Shape()
  shape.moveTo(-span, 0.10)
  shape.lineTo(-0.12, 0)
  shape.lineTo(span, 0)
  shape.lineTo(0.17, 0.14)
  shape.lineTo(0.09, 0.63)
  shape.lineTo(-0.10, 0.59)
  shape.lineTo(-0.16, 0.46)
  shape.closePath()
  const hole = new Path().absellipse(-0.01, 0.29, 0.055, 0.11, 0, Math.PI * 2, true, 0)
  shape.holes.push(hole)

  const screwHead = new Shape().absellipse(-0.01, 0.29, 0.049, 0.096, 0, Math.PI * 2, false, 0)
  const socket = new Path()
  for (let i = 0; i < 6; i += 1) {
    const angle = -i * Math.PI / 3
    const x = -0.01 + 0.024 * Math.cos(angle), y = 0.29 + 0.047 * Math.sin(angle)
    if (i === 0) socket.moveTo(x, y)
    else socket.lineTo(x, y)
  }
  socket.closePath()
  screwHead.holes.push(socket)

  // Local shape X follows the tangent, Y is axial, and extrusion points into the pocket.
  const place = new Matrix4().set(0, 0, -0.19 * radius, outside, -radius, 0, 0, 0,
    0, length, 0, 0, 0, 0, 0, 1)
  const inserts: BufferGeometry[] = []
  const screws: BufferGeometry[] = []
  for (let i = 0; i < insertCount; i += 1) {
    const angle = i * 2 * Math.PI / insertCount
    inserts.push(new ExtrudeGeometry(shape, { bevelEnabled: false, depth: 1, curveSegments: 8 })
      .applyMatrix4(place).rotateZ(angle))
    screws.push(new ExtrudeGeometry(screwHead, { bevelEnabled: false, depth: 0.16, curveSegments: 8 })
      .translate(0, 0, 0.05).applyMatrix4(place).rotateZ(angle))
  }
  return { body, inserts, screws }
}

function sweptGeometry(rings: readonly Ring[], contour: Contour, repeats: number): BufferGeometry {
  const around = contour.length * repeats
  const [bottom] = rings
  const top = rings.at(-1)
  if (!bottom || !top) throw new Error('Missing cutter profile')
  const tip = bottom.radius === 0
  const positions: number[] = []
  const indices: number[] = []
  const sides = tip ? rings.slice(1) : rings
  for (const { radius, z, phase, depth } of sides) {
    for (let tooth = 0; tooth < repeats; tooth += 1) {
      for (const [fraction, ratio] of contour) {
        const angle = (tooth + fraction) * 2 * Math.PI / repeats + phase
        const r = radius * (1 - (1 - ratio) * depth)
        positions.push(r * Math.cos(angle), r * Math.sin(angle), z)
      }
    }
  }
  const last = sides.length - 1
  for (let row = 0; row < last; row += 1) {
    const base = row * around
    for (let i = 0; i < around; i += 1) {
      const a = base + i, b = base + (i + 1) % around
      indices.push(a, b, a + around, b, b + around, a + around)
    }
  }

  const addCap = (row: number, z: number, top: boolean, flat: boolean): void => {
    let rim = row * around
    // Duplicate only flat rims to keep their normals separate from the flutes.
    if (flat) {
      const offset = rim * 3
      rim = positions.length / 3
      positions.push(...positions.slice(offset, offset + around * 3))
    }
    const centre = positions.length / 3
    positions.push(0, 0, z)
    for (let i = 0; i < around; i += 1) {
      const a = rim + i, b = rim + (i + 1) % around
      if (top) indices.push(centre, a, b)
      else indices.push(centre, b, a)
    }
  }
  addCap(0, bottom.z, false, !tip)
  addCap(last, top.z, true, true)

  const geometry = new BufferGeometry()
  geometry.setAttribute('position', new Float32BufferAttribute(positions, 3))
  geometry.setIndex(indices)
  geometry.computeVertexNormals()
  return geometry
}
