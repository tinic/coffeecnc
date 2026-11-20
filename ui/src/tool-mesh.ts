import { BufferGeometry, CylinderGeometry, Group, LatheGeometry, LineBasicMaterial, LineLoop, Mesh,
  MeshStandardMaterial, PMREMGenerator, SphereGeometry, Vector2, Vector3, type WebGLCubeRenderTarget, type WebGLRenderer } from 'three'
import { RoomEnvironment } from 'three/addons/environments/RoomEnvironment.js'
import { faceMillGeometry, flutedCutterGeometry } from './cutter-geometry'
import { holderSurfaces } from './tool-holder'
import { holderFaceZ, probeEnvelope, toolProfiles, type ProbeDimensions, type ToolModel } from './tool-model'

/** Shared visual envelope for the editor and the machine scene. */
export class ToolMesh {
  readonly group = new Group()
  // Ground, uncoated tooling is the default visual finish. Coating is not in the tool table.
  private readonly cutting = new MeshStandardMaterial({ color: '#969692', metalness: 0.87, roughness: 0.29, envMapIntensity: 0.75 })
  private readonly shank = new MeshStandardMaterial({ color: '#a9a9a6', metalness: 0.89, roughness: 0.24, envMapIntensity: 0.75 })
  private readonly groundSteel = new MeshStandardMaterial({ color: '#c8c8c6', metalness: 0.94, roughness: 0.25, envMapIntensity: 0.78 })
  private readonly darkSteel = new MeshStandardMaterial({ color: '#343435', metalness: 0.62, roughness: 0.42, envMapIntensity: 0.85 })
  private readonly housing = new MeshStandardMaterial({ color: '#454749', metalness: 0.45, roughness: 0.38 })
  private readonly ruby = new MeshStandardMaterial({ color: '#d32643', metalness: 0.08, roughness: 0.23 })
  private readonly surfaces = [this.cutting, this.shank, this.groundSteel, this.darkSteel, this.housing, this.ruby]
  private readonly datum = new LineBasicMaterial({ color: '#377ef2' })
  private readonly geometry: BufferGeometry[] = []
  private reflections: WebGLCubeRenderTarget | null = null
  private lightingReady = false

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
        this.addMesh(cutter ?? this.lathe(profiles.cutting), this.cutting, 'cutter')
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
    for (const screw of screws) this.addMesh(screw, this.housing, 'insert screw', head)
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
    cylinder(d.bodyDiameter, d.bodyLength, bodyStart, this.housing)
    if (showShank) cylinder(d.shankDiameter, d.shankDiameter, bodyEnd, this.shank)
  }

  light(renderer: WebGLRenderer): void {
    if (this.lightingReady) return
    this.lightingReady = true
    // Direct scene lighting still works when the GPU can't render a half-float reflection map.
    if (!renderer.extensions.has('EXT_color_buffer_float') && !renderer.extensions.has('EXT_color_buffer_half_float')) return
    const generator = new PMREMGenerator(renderer)
    const room = new RoomEnvironment()
    try {
      this.reflections = generator.fromScene(room, 0.03, 0.1, 100, { size: 128 })
      for (const material of this.surfaces) {
        material.envMap = this.reflections.texture
        material.needsUpdate = true
      }
    } finally {
      room.dispose()
      generator.dispose()
    }
  }

  resetLighting(): void {
    this.lightingReady = false
    this.reflections?.dispose()
    this.reflections = null
    for (const material of this.surfaces) {
      material.envMap = null
      material.needsUpdate = true
    }
  }

  setTheme(dark: boolean): void { this.datum.color.set(dark ? '#84b1f8' : '#377ef2') }

  dispose(): void {
    this.clear()
    this.resetLighting()
    this.surfaces.forEach((material) => { material.dispose() })
    this.datum.dispose()
  }
}
