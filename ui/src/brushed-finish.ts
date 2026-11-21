import { DataTexture, LinearFilter, LinearMipmapLinearFilter, RepeatWrapping, SRGBColorSpace } from 'three'

/** Fine milling passes for meshes with UV coordinates in millimetres. */
export function brushedFinish(): DataTexture {
  const size = 64
  const pixels = new Uint8Array(size * size * 4)
  for (let y = 0; y < size; y++) {
    const phase = 2 * Math.PI * y / size
    const shade = Math.round(247 + 3 * Math.cos(12 * phase) + 2 * Math.cos(19 * phase) + Math.cos(25 * phase))
    for (let x = 0; x < size; x++) pixels.set([shade, shade, shade, 255], 4 * (y * size + x))
  }
  const texture = new DataTexture(pixels, size, size)
  texture.colorSpace = SRGBColorSpace
  texture.wrapS = texture.wrapT = RepeatWrapping
  texture.repeat.set(1 / 48, 1 / 18)
  texture.magFilter = LinearFilter
  texture.minFilter = LinearMipmapLinearFilter
  texture.generateMipmaps = true
  texture.needsUpdate = true
  return texture
}
