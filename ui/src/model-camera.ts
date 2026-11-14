import type { OrthographicCamera } from 'three'
import { basis, type Camera } from './pathview'

/** Match the existing toolpath canvas exactly, including orbit, pan and pixels/mm. */
export function syncModelCamera(target: OrthographicCamera, camera: Camera, width: number, height: number, extent: number): void {
  const { right, up } = basis(camera)
  const direction = [right[1] * up[2] - right[2] * up[1], right[2] * up[0] - right[0] * up[2], right[0] * up[1] - right[1] * up[0]]
  const distance = Math.max(100, extent + 100)
  target.left = -width / (2 * camera.scale)
  target.right = -target.left
  target.top = height / (2 * camera.scale)
  target.bottom = -target.top
  target.near = 0.1
  target.far = distance * 2 + 100
  target.position.set(camera.centre[0] + (direction[0] ?? 0) * distance,
    camera.centre[1] + (direction[1] ?? 0) * distance, camera.centre[2] + (direction[2] ?? 0) * distance)
  target.up.set(...up)
  target.lookAt(...camera.centre)
  target.updateProjectionMatrix()
  target.updateMatrixWorld()
}
