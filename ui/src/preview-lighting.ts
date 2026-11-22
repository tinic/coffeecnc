import { Color, DirectionalLight, HemisphereLight, Mesh, MeshBasicMaterial, NeutralToneMapping, PlaneGeometry,
  PMREMGenerator, Scene, type WebGLCubeRenderTarget, type WebGLRenderer } from 'three'

/** Direct light remains usable on GPUs without renderable floating-point textures. */
export function lightPreview(scene: Scene, renderer: WebGLRenderer): DirectionalLight {
  renderer.setClearColor(0, 0)
  renderer.toneMapping = NeutralToneMapping
  renderer.toneMappingExposure = 0.88
  const key = new DirectionalLight(0xfff7ef, 3.1)
  key.position.set(-56, -38, 76)
  const rim = new DirectionalLight(0xd9eaff, 0.95)
  rim.position.set(48, 28, 20)
  const ambient = new HemisphereLight(0xe9eef4, 0x555b65, 0.53)
  ambient.position.set(0, 0, 1)
  scene.add(ambient, key, rim)
  return key
}

/** Rectangular studio lights reflected in the tooling and machined surfaces. */
export class PreviewReflections {
  private target: WebGLCubeRenderTarget | null = null

  build(scene: Scene, renderer: WebGLRenderer): void {
    this.dispose(scene)
    if (!renderer.extensions.has('EXT_color_buffer_float') && !renderer.extensions.has('EXT_color_buffer_half_float')) return

    const studio = new Scene()
    studio.background = new Color('#363f49')
    const panelGeometry = new PlaneGeometry(1, 1)
    const panelMaterials: MeshBasicMaterial[] = []
    const softbox = (position: [number, number, number], size: [number, number], colour: string, intensity: number): void => {
      const material = new MeshBasicMaterial({ color: new Color(colour).multiplyScalar(intensity), toneMapped: false })
      panelMaterials.push(material)
      const panel = new Mesh(panelGeometry, material)
      panel.position.set(...position)
      panel.scale.set(...size, 1)
      panel.lookAt(0, 0, 0)
      studio.add(panel)
    }
    softbox([-6, -4, 1.5], [3.2, 13], '#fff6eb', 4.0)
    softbox([5, -5, 1.5], [2.6, 12], '#deebfa', 2.5)
    softbox([4, 5, 2], [1.5, 12], '#f3f7fd', 5.2)
    softbox([-1, 2, 6], [9, 5.5], '#edf3fb', 2.8)
    softbox([0, -2, -6], [9, 8], '#c0c4cc', 0.8)
    const generator = new PMREMGenerator(renderer)
    try {
      this.target = generator.fromScene(studio, 0.028, 0.1, 40, { size: 128 })
      scene.environment = this.target.texture
    } finally {
      panelGeometry.dispose()
      panelMaterials.forEach(material => { material.dispose() })
      generator.dispose()
    }
  }

  dispose(scene: Scene): void {
    scene.environment = null
    this.target?.dispose()
    this.target = null
  }
}
