import { BufferGeometry, CylinderGeometry, Group, LatheGeometry, LineBasicMaterial, LineLoop, Mesh, MeshStandardMaterial, SphereGeometry, Vector2, Vector3 } from 'three'
import { holderProfile } from './tool-holder'
import { holderFaceZ, probeEnvelope, toolProfiles, type ProbeDimensions, type ToolModel } from './tool-model'

/** Shared visual envelope for the editor and the machine scene. */
export class ToolMesh {
  readonly group = new Group()
  private readonly cutting = new MeshStandardMaterial({ color: '#bc9954', metalness: 0.5, roughness: 0.36 })
  private readonly shank = new MeshStandardMaterial({ color: '#a4b2be', metalness: 0.65, roughness: 0.28 })
  private readonly housing = new MeshStandardMaterial({ color: '#416077', metalness: 0.4, roughness: 0.34 })
  private readonly ruby = new MeshStandardMaterial({ color: '#d32643', metalness: 0.08, roughness: 0.23 })
  private readonly datum = new LineBasicMaterial({ color: '#377ef2' })
  private readonly geometry: BufferGeometry[] = []

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
      this.addProfile(profiles.cutting, this.cutting, 'cutter')
      this.addProfile(profiles.shank, this.shank, 'shank')
    }
    this.addProfile(holderProfile(model.holder, holderFaceZ(d), d.shankDiameter), this.housing, 'holder')
    if ('bodyDiameter' in d) return
    const ring = new BufferGeometry().setFromPoints(Array.from({ length: 96 }, (_, i) => {
      const angle = i * Math.PI / 48
      const radius = Math.max(d.shankDiameter * 0.7, d.diameter * 0.6)
      return new Vector3(Math.cos(angle) * radius, Math.sin(angle) * radius, d.stickout)
    }))
    this.geometry.push(ring)
    this.group.add(new LineLoop(ring, this.datum))
  }

  private addProfile(profile: readonly [number, number][], material: MeshStandardMaterial, name: string): void {
    if (profile.length === 0) return
    const g = new LatheGeometry(profile.map(([radius, z]) => new Vector2(radius, z)), 64)
    g.rotateX(Math.PI / 2) // Lathe's +Y axis becomes the tool's +Z axis.
    this.geometry.push(g)
    const mesh = new Mesh(g, material)
    mesh.name = name
    this.group.add(mesh)
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

  setTheme(dark: boolean): void { this.datum.color.set(dark ? '#84b1f8' : '#377ef2') }
  dispose(): void { this.clear(); this.cutting.dispose(); this.shank.dispose(); this.housing.dispose(); this.ruby.dispose(); this.datum.dispose() }
}
