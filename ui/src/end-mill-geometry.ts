import { BufferGeometry, Float32BufferAttribute } from 'three'

// One nominal tooth, from the cutting edge through the relief and flute valley.
const tooth = [
  [0, 1], [0.07, 0.99], [0.15, 0.95], [0.24, 0.83], [0.34, 0.66],
  [0.44, 0.59], [0.54, 0.57], [0.64, 0.58], [0.74, 0.62], [0.82, 0.69],
  [0.89, 0.78], [0.94, 0.88], [0.98, 0.96],
] as const
const flutes = 3
const around = tooth.length * flutes
const along = 64

export function endMillGeometry(diameter: number, length: number): BufferGeometry {
  const radius = diameter / 2
  // A 30° right-hand helix at the cutting edge. Cap turns on extreme proportions
  // so the preview mesh still resolves the flutes between consecutive rings.
  const twist = Math.min(length / (Math.sqrt(3) * radius), 6 * Math.PI)
  const positions: number[] = []
  const indices: number[] = []
  for (let row = 0; row <= along; row += 1) {
    const t = row / along
    const runout = Math.min(1, (1 - t) / 0.18)
    const depth = runout * runout * (3 - 2 * runout)
    for (let flute = 0; flute < flutes; flute += 1) {
      for (const [fraction, ratio] of tooth) {
        const theta = (flute + fraction) * 2 * Math.PI / flutes + twist * t
        const r = radius * (1 - (1 - ratio) * depth)
        positions.push(r * Math.cos(theta), r * Math.sin(theta), length * t)
      }
    }
  }
  for (let row = 0; row < along; row += 1) {
    const base = row * around
    for (let i = 0; i < around; i += 1) {
      const a = base + i
      const b = base + (i + 1) % around
      indices.push(a, b, a + around, b, b + around, a + around)
    }
  }
  // Duplicate the rim vertices so the end faces don't smooth into the flutes.
  for (const row of [0, along]) {
    const cap = positions.length / 3
    const offset = row * around * 3
    positions.push(...positions.slice(offset, offset + around * 3))
    const centre = positions.length / 3
    positions.push(0, 0, row === 0 ? 0 : length)
    for (let i = 0; i < around; i += 1) {
      const next = cap + (i + 1) % around
      if (row === 0) indices.push(centre, next, cap + i)
      else indices.push(centre, cap + i, next)
    }
  }

  const geometry = new BufferGeometry()
  geometry.setAttribute('position', new Float32BufferAttribute(positions, 3))
  geometry.setIndex(indices)
  geometry.computeVertexNormals()
  return geometry
}
