import { DirectionalLight, HemisphereLight, NeutralToneMapping, type Scene, type WebGLRenderer } from 'three'

/** Studio lighting for the live scene and the standalone tool preview. */
export function lightPreview(scene: Scene, renderer: WebGLRenderer): void {
  renderer.setClearColor(0, 0)
  renderer.toneMapping = NeutralToneMapping
  renderer.toneMappingExposure = 0.95
  scene.environmentIntensity = 0.68
  const key = new DirectionalLight(0xfff4e7, 2.25)
  key.position.set(-240, -140, 330)
  const rim = new DirectionalLight(0xd9eaff, 0.92)
  rim.position.set(160, 220, 105)
  scene.add(new HemisphereLight(0xeef4fd, 0x8b8180, 1.1), key, rim)
}
