import { BoxGeometry, BufferGeometry, CylinderGeometry, ExtrudeGeometry, Group, Mesh, MeshStandardMaterial, Path, Shape } from 'three'
import type { Vec3 } from '../protocol'
import { demoFixture, type ProbeBlock } from './probe-fixture'

export function buildProbeFixture(): { group: Group; dispose: () => void } {
  const group = new Group()
  const geometries: BufferGeometry[] = []
  const aluminium = new MeshStandardMaterial({ color: '#849bae', metalness: 0.29, roughness: 0.55 })
  const gaugeSteel = new MeshStandardMaterial({ color: '#a8b6ba', metalness: 0.42, roughness: 0.42 })
  const black = new MeshStandardMaterial({ color: '#3a4653', metalness: 0.34, roughness: 0.42 })
  const gold = new MeshStandardMaterial({ color: '#eab750', metalness: 0.39, roughness: 0.31 })
  const { stock, slanted, gauge, bore, boss, setter, top, stockHeight, boreRadius, bossRadius, setterHeight, setterRadius } = demoFixture
  const add = (geometry: BufferGeometry, material: MeshStandardMaterial, centre: Vec3, angle = 0): void => {
    const mesh = new Mesh(geometry, material)
    mesh.position.set(...centre)
    mesh.rotation.z = angle * Math.PI / 180
    group.add(mesh)
    geometries.push(geometry)
  }
  const block = (item: ProbeBlock, material: MeshStandardMaterial): void => {
    add(new BoxGeometry(item.half[0] * 2, item.half[1] * 2, stockHeight), material,
      [item.centre[0], item.centre[1], top - stockHeight / 2], item.angle)
  }
  const cylinder = (radius: number, height: number, centre: Vec3, material: MeshStandardMaterial): void => {
    add(new CylinderGeometry(radius, radius, height, 48).rotateX(Math.PI / 2), material, centre)
  }

  const [x, y] = stock.half
  const shape = new Shape().moveTo(-x, -y).lineTo(x, -y).lineTo(x, y).lineTo(-x, y).closePath()
  shape.holes.push(new Path().absarc(bore[0] - stock.centre[0], bore[1] - stock.centre[1], boreRadius, 0, 2 * Math.PI, true))
  add(new ExtrudeGeometry(shape, { depth: stockHeight, bevelEnabled: false, curveSegments: 32 }),
    aluminium, [stock.centre[0], stock.centre[1], top - stockHeight])
  block(slanted, aluminium)
  block(gauge, gaugeSteel)
  cylinder(bossRadius, stockHeight, [boss[0], boss[1], top - stockHeight / 2], aluminium)
  cylinder(setterRadius, setterHeight - 3, [setter[0], setter[1], setter[2] - (setterHeight + 3) / 2], black)
  cylinder(setterRadius - 2.5, 3, [setter[0], setter[1], setter[2] - 1.5], gold)

  return { group, dispose: (): void => {
    group.clear()
    for (const geometry of geometries) geometry.dispose()
    for (const material of [aluminium, gaugeSteel, black, gold]) material.dispose()
  } }
}
