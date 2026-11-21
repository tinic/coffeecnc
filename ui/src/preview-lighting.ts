import { Color, DirectionalLight, HemisphereLight, Mesh, MeshBasicMaterial, NeutralToneMapping, PlaneGeometry,
  PMREMGenerator, Scene, type WebGLCubeRenderTarget, type WebGLRenderer } from 'three'

/** Direct light remains usable on GPUs without renderable floating-point textures. */
export function lightPreview(scene: Scene, renderer: WebGLRenderer): DirectionalLight {
  renderer.setClearColor(0, 0)
  renderer.toneMapping = NeutralToneMapping
  renderer.toneMappingExposure = 0.93
  const key = new DirectionalLight(0xfff3e5, 2.4)
  key.position.set(-42, -50, 72)
  const rim = new DirectionalLight(0xdcecff, 1.15)
  rim.position.set(48, 44, 32)
  const ambient = new HemisphereLight(0xeaf3fd, 0x5b6372, 0.85)
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
    studio.background = new Color('#626b78')
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
    softbox([-5, -6, 2], [5.2, 12], '#fff4e8', 4.4)
    softbox([6, -1, 2], [1.6, 11], '#e1edff', 3.2)
    softbox([1, 4, 6], [8, 3.3], '#f3f8ff', 2.6)
    softbox([-1, -1, -6], [9, 8], '#afbbc9', 0.6)
    const generator = new PMREMGenerator(renderer)
    try {
      this.target = generator.fromScene(studio, 0.045, 0.1, 40, { size: 128 })
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
