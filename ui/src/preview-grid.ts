import { project, type Camera, type Screen } from './pathview'
import type { Vec3 } from './protocol'

/** Draw the XY work plane behind the scene, with enough spacing to stay legible when zoomed out. */
export function drawPreviewGrid(context: CanvasRenderingContext2D, camera: Camera, origin: Vec3,
  angle: number, width: number, height: number, dark: boolean): void {
  const elevation = Math.sin(camera.elevation)
  if (elevation <= 0.1) return

  const [ox, oy] = project(camera, origin, width, height)
  const relativeAngle = camera.azimuth - angle
  const xx = camera.scale * Math.cos(relativeAngle)
  const xy = camera.scale * Math.sin(relativeAngle) * elevation
  const yx = camera.scale * Math.sin(relativeAngle)
  const yy = -camera.scale * Math.cos(relativeAngle) * elevation
  const determinant = xx * yy - xy * yx
  const targetSpacing = 26 / (camera.scale * elevation)
  const decade = 10 ** Math.floor(Math.log10(targetSpacing))
  const spacing = decade * ([1, 2, 5, 10].find(step => step * decade >= targetSpacing) ?? 10)

  const corners: Screen[] = [[0, 0], [width, 0], [width, height], [0, height]]
  // Invert the projected plane so pans, rotations and large machine offsets still fill the viewport.
  const xs = corners.map(([x, y]) => ((x - ox) * yy - (y - oy) * yx) / determinant)
  const ys = corners.map(([x, y]) => ((y - oy) * xx - (x - ox) * xy) / determinant)
  const x0 = Math.floor(Math.min(...xs) / spacing), x1 = Math.ceil(Math.max(...xs) / spacing)
  const y0 = Math.floor(Math.min(...ys) / spacing), y1 = Math.ceil(Math.max(...ys) / spacing)
  if (![x0, x1, y0, y1].every(Number.isSafeInteger)) return
  const line = (x: number, y: number, endX: number, endY: number): void => {
    context.moveTo(ox + spacing * (x * xx + y * yx), oy + spacing * (x * xy + y * yy))
    context.lineTo(ox + spacing * (endX * xx + endY * yx), oy + spacing * (endX * xy + endY * yy))
  }
  context.strokeStyle = dark ? '#9cbbd0' : '#59788e'
  context.lineWidth = 1
  const fade = Math.min(1, (elevation - 0.1) / 0.26)
  for (const major of [false, true]) {
    context.globalAlpha = fade * (major ? 0.16 : 0.08)
    context.beginPath()
    for (let x = x0; x <= x1; x++) if ((x % 5 === 0) === major) line(x, y0, x, y1)
    for (let y = y0; y <= y1; y++) if ((y % 5 === 0) === major) line(x0, y, x1, y)
    context.stroke()
  }
  context.globalAlpha = 1
}
