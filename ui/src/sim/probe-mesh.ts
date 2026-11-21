import { BufferGeometry, CylinderGeometry, DataTexture, ExtrudeGeometry, Group, LinearFilter, Mesh,
  MeshBasicMaterial, MeshStandardMaterial, Path, PlaneGeometry, Shape } from 'three'
import { brushedFinish } from '../brushed-finish'
import type { Vec3 } from '../protocol'
import { demoFixture, type ProbeBlock } from './probe-fixture'

/** Small, fixed grounding shadows for the demo shapes. No shadow pass on each live tool update. */
function contactShadow(round: boolean): DataTexture {
  const size = 64
  const rgba = new Uint8Array(size * size * 4)
  for (let y = 0; y < size; y++) {
    for (let x = 0; x < size; x++) {
      const u = 2 * (x + 0.5) / size - 1, v = 2 * (y + 0.5) / size - 1
      const distance = round ? Math.hypot(u, v) : Math.max(Math.abs(u), Math.abs(v))
      const coverage = Math.max(0, Math.min(1, (0.98 - distance) / 0.42))
      rgba[4 * (y * size + x) + 3] = Math.round(255 * coverage * coverage)
    }
  }
  const texture = new DataTexture(rgba, size, size)
  texture.magFilter = texture.minFilter = LinearFilter
  texture.needsUpdate = true
  return texture
}

export function buildProbeFixture(): { group: Group; dispose: () => void } {
  const group = new Group()
  const geometries: BufferGeometry[] = []
  const finish = brushedFinish()
  const aluminium = new MeshStandardMaterial({ color: '#b9c0c5', metalness: 0.61, roughness: 0.36 })
  const milledTop = aluminium.clone()
  milledTop.map = finish
  const milledSides = new MeshStandardMaterial({ color: '#a0a8ad', metalness: 0.58, roughness: 0.42 })
  const gaugeSteel = new MeshStandardMaterial({ color: '#b5bdc5', metalness: 0.79, roughness: 0.27 })
  const black = new MeshStandardMaterial({ color: '#394047', metalness: 0.45, roughness: 0.39 })
  const gold = new MeshStandardMaterial({ color: '#d3a055', metalness: 0.59, roughness: 0.3 })
  const shadows = [false, true].map(round => new MeshBasicMaterial({
    map: contactShadow(round), transparent: true, depthWrite: false, opacity: 0.38, toneMapped: false,
  }))
  const { stock, slanted, gauge, bore, boss, setter, top, stockHeight, boreRadius, bossRadius, setterHeight, setterRadius } = demoFixture
  const add = (geometry: BufferGeometry, material: Mesh['material'], centre: Vec3, angle = 0): void => {
    const mesh = new Mesh(geometry, material)
    mesh.position.set(...centre)
    mesh.rotation.z = angle * Math.PI / 180
    group.add(mesh)
    geometries.push(geometry)
  }
  const shadow = (halfWidth: number, halfHeight: number, position: Vec3, angle: number, round = false): void => {
    const material = shadows[round ? 1 : 0]
    if (material) add(new PlaneGeometry(halfWidth * 2.85, halfHeight * 2.85), material, position, angle)
  }
  const block = (item: ProbeBlock, materials: MeshStandardMaterial[], hole = false): void => {
    const bevel = 0.55
    const x = item.half[0] - bevel, y = item.half[1] - bevel
    const shape = new Shape().moveTo(-x, -y).lineTo(x, -y).lineTo(x, y).lineTo(-x, y).closePath()
    if (hole) shape.holes.push(new Path().absarc(bore[0] - item.centre[0], bore[1] - item.centre[1], boreRadius + bevel, 0, 2 * Math.PI, true))
    const base: Vec3 = [item.centre[0], item.centre[1], top - stockHeight]
    add(new ExtrudeGeometry(shape, { depth: stockHeight - 2 * bevel, bevelSize: bevel, bevelThickness: bevel,
      bevelSegments: 2, curveSegments: 48 }), materials, [base[0], base[1], base[2] + bevel], item.angle)
    shadow(...item.half, base, item.angle)
  }
  const cylinder = (radius: number, height: number, centre: Vec3, material: MeshStandardMaterial): void => {
    add(new CylinderGeometry(radius, radius, height, 48).rotateX(Math.PI / 2), material, centre)
  }

  block(stock, [milledTop, milledSides], true)
  block(slanted, [milledTop, milledSides])
  block(gauge, [gaugeSteel, gaugeSteel])
  cylinder(bossRadius, stockHeight, [boss[0], boss[1], top - stockHeight / 2], aluminium)
  shadow(bossRadius, bossRadius, [boss[0], boss[1], top - stockHeight], 0, true)
  cylinder(setterRadius, setterHeight - 3, [setter[0], setter[1], setter[2] - (setterHeight + 3) / 2], black)
  cylinder(setterRadius - 2.5, 3, [setter[0], setter[1], setter[2] - 1.5], gold)
  shadow(setterRadius, setterRadius, [setter[0], setter[1], setter[2] - setterHeight], 0, true)

  return { group, dispose: (): void => {
    group.clear()
    for (const geometry of geometries) geometry.dispose()
    finish.dispose()
    for (const material of [aluminium, milledTop, milledSides, gaugeSteel, black, gold]) material.dispose()
    for (const material of shadows) {
      material.map?.dispose()
      material.dispose()
    }
  } }
}
