import { BufferGeometry, Group, LatheGeometry, LineBasicMaterial, LineLoop, Mesh, MeshStandardMaterial, Vector2, Vector3 } from 'three'
import { toolProfiles, type ToolModel } from './tool-model'

/** Shared visual envelope for the editor and the machine scene. */
export class ToolMesh {
  readonly group = new Group()
  private readonly cutting = new MeshStandardMaterial({ color: '#bc9954', metalness: 0.5, roughness: 0.36 })
  private readonly shank = new MeshStandardMaterial({ color: '#a4b2be', metalness: 0.65, roughness: 0.28 })
  private readonly datum = new LineBasicMaterial({ color: '#377ef2' })
  private readonly geometry: BufferGeometry[] = []

  clear(): void {
    this.group.clear()
    this.geometry.splice(0).forEach((g) => { g.dispose() })
  }

  build(model: ToolModel): void {
    this.clear()
    const profiles = toolProfiles(model)
    for (const [profile, material] of [[profiles.cutting, this.cutting], [profiles.shank, this.shank]] as const) {
      const g = new LatheGeometry(profile.map(([radius, z]) => new Vector2(radius, z)), 64)
      g.rotateX(Math.PI / 2) // Lathe's +Y axis becomes the tool's +Z axis.
      this.geometry.push(g)
      this.group.add(new Mesh(g, material))
    }
    const d = model.dimensions
    const ring = new BufferGeometry().setFromPoints(Array.from({ length: 96 }, (_, i) => {
      const angle = i * Math.PI / 48
      const radius = Math.max(d.shankDiameter * 0.7, d.diameter * 0.6)
      return new Vector3(Math.cos(angle) * radius, Math.sin(angle) * radius, d.stickout)
    }))
    this.geometry.push(ring)
    this.group.add(new LineLoop(ring, this.datum))
  }

  setTheme(dark: boolean): void { this.datum.color.set(dark ? '#84b1f8' : '#377ef2') }
  dispose(): void { this.clear(); this.cutting.dispose(); this.shank.dispose(); this.datum.dispose() }
}
