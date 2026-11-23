import { BufferGeometry, CylinderGeometry, Float32BufferAttribute, Group, LatheGeometry, LineBasicMaterial, LineLoop, Mesh,
  MeshPhysicalMaterial, MeshStandardMaterial, SphereGeometry, Vector2, Vector3 } from 'three'
import { faceMillGeometry, flutedCutterGeometry } from './cutter-geometry'
import { holderSurfaces } from './tool-holder'
import { holderFaceZ, probeEnvelope, shankProfile, toolProfiles, type ProbeDimensions, type ToolModel } from './tool-model'

/** Shared visual envelope for the editor and the machine scene. */
export class ToolMesh {
  readonly group = new Group()
  // Ground, uncoated tooling is the default visual finish. Coating is not in the tool table.
  private readonly cutting = new MeshStandardMaterial({ color: '#b1ada7', metalness: 0.79, roughness: 0.27 })
  private readonly flutes = this.cutting.clone()
  private readonly shank = new MeshStandardMaterial({ color: '#aeb8c0', metalness: 0.88, roughness: 0.26, envMapIntensity: 0.8 })
  private readonly groundSteel = new MeshStandardMaterial({ color: '#c0c7cc', metalness: 0.84, roughness: 0.29 })
  private readonly darkSteel = new MeshStandardMaterial({ color: '#343a40', metalness: 0.58, roughness: 0.4 })
  private readonly housing = new MeshStandardMaterial({ color: '#525a62', metalness: 0.63, roughness: 0.32, vertexColors: true })
  private readonly ruby = new MeshPhysicalMaterial({ color: '#b50836', roughness: 0.27,
    clearcoat: 1, clearcoatRoughness: 0.13, emissive: '#70001d', emissiveIntensity: 0.15 })
  private readonly surfaces = [this.cutting, this.flutes, this.shank, this.groundSteel, this.darkSteel, this.housing, this.ruby]
  private readonly datum = new LineBasicMaterial({ color: '#377ef2', transparent: true, opacity: 0.72, toneMapped: false })
  private readonly geometry: BufferGeometry[] = []

  constructor() { this.flutes.vertexColors = true }

  clear(): void {
    this.group.clear()
    this.geometry.splice(0).forEach((g) => { g.dispose() })
  }

  build(model: ToolModel): void {
    this.clear()
    const d = model.dimensions
    if ('bodyDiameter' in d) this.buildProbe(d, model.holder === undefined)
    else {
      const profiles = toolProfiles(model)
      if (model.family === 'face-mill') this.buildFaceMill(d.diameter, d.cuttingLength)
      else {
        const cutter = flutedCutterGeometry(model.family, profiles.cutting, d.diameter, d.cuttingLength)
        this.addMesh(cutter ?? this.lathe(profiles.cutting), cutter ? this.flutes : this.cutting, 'cutter')
      }
      if (profiles.shank.length) this.addMesh(this.lathe(profiles.shank), this.shank, 'shank')
    }
    this.buildHolder(model)
    if ('bodyDiameter' in d) return
    const ring = new BufferGeometry().setFromPoints(Array.from({ length: 96 }, (_, i) => {
      const angle = i * Math.PI / 48
      const radius = Math.max(d.shankDiameter * 0.7, d.diameter * 0.6)
      return new Vector3(Math.cos(angle) * radius, Math.sin(angle) * radius, d.stickout)
    }))
    this.geometry.push(ring)
    this.group.add(new LineLoop(ring, this.datum))
  }

  private lathe(profile: readonly [number, number][]): LatheGeometry {
    return new LatheGeometry(profile.map(([radius, z]) => new Vector2(radius, z)), 96)
      .rotateX(Math.PI / 2) // Lathe's +Y axis becomes the tool's +Z axis.
  }

  private addMesh(g: BufferGeometry, material: MeshStandardMaterial, name: string, parent = this.group): void {
    this.geometry.push(g)
    const mesh = new Mesh(g, material)
    mesh.name = name
    parent.add(mesh)
  }

  private buildFaceMill(diameter: number, length: number): void {
    const head = new Group()
    head.name = 'cutter'
    const { body, inserts, screws } = faceMillGeometry(diameter, length)
    this.addMesh(body, this.darkSteel, 'head', head)
    for (const insert of inserts) this.addMesh(insert, this.cutting, 'insert', head)
    for (const screw of screws) this.addMesh(screw, this.darkSteel, 'insert screw', head) // i18n-ignore
    this.group.add(head)
  }

  private buildHolder(model: ToolModel): void {
    const d = model.dimensions
    const surfaces = holderSurfaces(model.holder, holderFaceZ(d), d.shankDiameter)
    if (surfaces.length === 0) return
    const holder = new Group()
    holder.name = 'holder'
    for (const { finish, profile, wrenchFlats } of surfaces) {
      const geometry = this.lathe(profile)
      if (wrenchFlats) {
        const { face, radius, apothem, length } = wrenchFlats
        const position = geometry.getAttribute('position')
        const sector = Math.PI / 3
        for (let i = 0; i < position.count; i += 1) {
          const x = position.getX(i), y = position.getY(i), z = position.getZ(i)
          const angle = Math.atan2(y, x)
          const flatAngle = angle - Math.floor(angle / sector) * sector - sector / 2
          // The wrench flats fade into the round rear portion of the nut.
          const t = Math.max(0, Math.min(1, ((z - face) / length - 0.57) / 0.21))
          const limit = (apothem + (radius - apothem) * t) / Math.cos(flatAngle)
          const r = Math.hypot(x, y)
          if (r > limit) position.setXY(i, x * limit / r, y * limit / r)
        }
        geometry.computeVertexNormals()
      }
      this.addMesh(geometry, finish === 'ground' ? this.groundSteel : this.darkSteel, '', holder)
    }
    this.group.add(holder)
  }

  private buildProbe(d: ProbeDimensions, showShank: boolean): void {
    const { bodyStart, bodyEnd, stylusDiameter } = probeEnvelope(d)
    const ball = new SphereGeometry(d.diameter / 2, 40, 24)
    ball.translate(0, 0, d.diameter / 2)
    this.geometry.push(ball)
    this.group.add(new Mesh(ball, this.ruby))
    const cylinder = (diameter: number, length: number, start: number, material: MeshStandardMaterial): void => {
      const g = new CylinderGeometry(diameter / 2, diameter / 2, length, 48)
      g.rotateX(Math.PI / 2)
      g.translate(0, 0, start + length / 2)
      this.geometry.push(g)
      this.group.add(new Mesh(g, material))
    }
    // Extend the stem into the ball so their surfaces intersect.
    cylinder(stylusDiameter, d.stylusLength + d.diameter / 4, d.diameter * 0.75, this.shank)
    const radius = d.bodyDiameter / 2
    const bevel = Math.min(radius * 0.08, d.bodyLength * 0.09)
    // The turned rims catch light around the darker probe housing.
    const rings: [radius: number, z: number, shade: number][] = [
      [0, bodyStart, 0.62], [radius - bevel, bodyStart, 0.74], [radius, bodyStart + bevel, 1],
      [radius, bodyStart + bevel * 1.65, 0.96], [radius - bevel * 0.16, bodyStart + bevel * 1.85, 0.42],
      [radius - bevel * 0.16, bodyEnd - bevel * 1.85, 0.42], [radius, bodyEnd - bevel * 1.65, 0.96],
      [radius, bodyEnd - bevel, 1], [radius - bevel, bodyEnd, 0.74], [0, bodyEnd, 0.62],
    ]
    const profile = this.lathe(rings.map(([r, z]) => [r, z]))
    const finishes = rings.flatMap(([, , shade]) => [shade, shade, shade])
    const colours = new Float32Array((profile.parameters.segments + 1) * finishes.length)
    for (let i = 0; i < colours.length; i += finishes.length) colours.set(finishes, i)
    profile.setAttribute('color', new Float32BufferAttribute(colours, 3))
    this.addMesh(profile, this.housing, 'probe housing') // i18n-ignore
    if (showShank) this.addMesh(this.lathe(shankProfile(d.shankDiameter / 2, bodyEnd, bodyEnd + d.shankDiameter)), this.shank, 'shank')
  }

  setTheme(dark: boolean): void { this.datum.color.set(dark ? '#84b1f8' : '#377ef2') }

  dispose(): void {
    this.clear()
    this.surfaces.forEach((material) => { material.dispose() })
    this.datum.dispose()
  }
}
