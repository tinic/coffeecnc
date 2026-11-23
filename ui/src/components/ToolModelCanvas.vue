<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref, useTemplateRef, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { Box3, OrthographicCamera, Scene, Vector3, WebGLRenderer } from 'three'
import { theme } from '../appearance'
import { isDarkTheme } from '../operator-settings'
import { syncModelCamera } from '../model-camera'
import { orbit, type Camera } from '../pathview'
import { lightPreview, PreviewReflections } from '../preview-lighting'
import type { ToolModel } from '../tool-model'
import { ToolMesh } from '../tool-mesh'
import { holderLabel } from '../tool-holder'

const props = defineProps<{ model: ToolModel }>()
const canvas = useTemplateRef<HTMLCanvasElement>('canvas')
const { t } = useI18n()
const error = ref<'' | 'lost' | 'webgl'>('')
const scene = new Scene()
const reflections = new PreviewReflections()
const toolMesh = new ToolMesh()
const camera = new OrthographicCamera()
const bounds = new Box3()
const centre = new Vector3()
const point = new Vector3()
const size = new Vector3()
let renderer: WebGLRenderer | null = null
let resize: ResizeObserver | null = null
let view: Camera = { azimuth: -0.68, elevation: -0.24, scale: 1, centre: [0, 0, 0] }
let drag: { id: number; x: number; y: number } | null = null
let contextLost = false

function build(): void {
  toolMesh.build(props.model)
  bounds.setFromObject(toolMesh.group)
  view.centre = bounds.getCenter(centre).toArray()
  draw()
}
function draw(): void {
  if (renderer === null || canvas.value === null || contextLost || bounds.isEmpty()) return
  const width = canvas.value.clientWidth; const height = canvas.value.clientHeight
  if (width <= 0 || height <= 0) return
  camera.zoom = 1
  syncModelCamera(camera, view, width, height, bounds.getSize(size).length())
  camera.rotateZ(0.52)
  camera.updateMatrixWorld()
  let x = 0; let y = 0
  const { min, max } = bounds
  for (const px of [min.x, max.x]) {
    for (const py of [min.y, max.y]) {
      for (const pz of [min.z, max.z]) {
        point.set(px, py, pz).project(camera)
        x = Math.max(x, Math.abs(point.x))
        y = Math.max(y, Math.abs(point.y))
      }
    }
  }
  camera.zoom = 0.93 / Math.max(x, y, 0.001)
  camera.updateProjectionMatrix()
  toolMesh.setTheme(isDarkTheme(theme.value))
  const ratio = Math.min(window.devicePixelRatio, 2)
  if (renderer.getPixelRatio() !== ratio || canvas.value.width !== Math.floor(width * ratio)
    || canvas.value.height !== Math.floor(height * ratio)) {
    renderer.setPixelRatio(ratio)
    renderer.setSize(width, height, false)
  }
  renderer.render(scene, camera)
}
function down(event: PointerEvent): void {
  if (event.button !== 0) return
  canvas.value?.setPointerCapture(event.pointerId)
  drag = { id: event.pointerId, x: event.clientX, y: event.clientY }
}
function move(event: PointerEvent): void {
  if (drag?.id !== event.pointerId) return
  view = orbit(view, event.clientX - drag.x, event.clientY - drag.y, -Math.PI / 2)
  drag.x = event.clientX; drag.y = event.clientY
  draw()
}
function lost(event: Event): void { event.preventDefault(); contextLost = true; error.value = 'lost' }
function restored(): void {
  contextLost = false
  if (renderer !== null) reflections.build(scene, renderer)
  error.value = ''
  draw()
}
watch(() => props.model, build, { deep: true })
watch(theme, draw)
onMounted(() => {
  if (canvas.value === null) return
  try {
    renderer = new WebGLRenderer({ canvas: canvas.value, alpha: true, antialias: true })
    lightPreview(scene, renderer)
    reflections.build(scene, renderer)
    scene.add(toolMesh.group)
    canvas.value.addEventListener('webglcontextlost', lost)
    canvas.value.addEventListener('webglcontextrestored', restored)
    resize = new ResizeObserver(draw)
    resize.observe(canvas.value)
    build()
  } catch { error.value = 'webgl' }
})
onBeforeUnmount(() => {
  resize?.disconnect()
  canvas.value?.removeEventListener('webglcontextlost', lost)
  canvas.value?.removeEventListener('webglcontextrestored', restored)
  toolMesh.dispose()
  reflections.dispose(scene)
  renderer?.dispose(); renderer?.forceContextLoss()
})
</script>

<template>
  <div class="model-canvas">
    <span class="caption label">{{ t('tools.canvas.caption') }} <span class="hint">· {{ t('tools.canvas.dragHint') }}</span></span>
    <canvas ref="canvas" :aria-label="t('tools.canvas.label')" @pointerdown="down" @pointermove="move" @pointerup="drag = null" @lostpointercapture="drag = null" @pointercancel="drag = null"></canvas>
    <p v-if="error" class="error blocked">{{ error === 'lost' ? t('tools.canvas.contextLost') : t('tools.canvas.needsWebgl') }}</p>
    <div v-if="model.family === 'touch-probe'" class="legend"><span class="ruby">{{ t('tools.canvas.rubyBall') }}</span></div>
    <div v-else class="legend"><span class="cutting">{{ t('tools.canvas.cuttingSection') }}</span><span class="datum">{{ t('tools.canvas.holderDatum') }}</span></div>
    <p class="hint foot">{{ model.family === 'touch-probe' ? t('tools.canvas.ballDatum') : t('tools.canvas.tipDatum') }}{{ model.holder ? ` · ${holderLabel(model.holder)}` : '' }}</p>
  </div>
</template>

<style scoped>
.model-canvas { position: relative; display: flex; flex-direction: column; min-height: 0; height: 100%; border: 1px solid var(--line); border-radius: 8px; background: var(--tool-preview-backdrop); overflow: hidden; }
.caption { padding: 12px 12px 0; pointer-events: none; }
canvas { width: 100%; flex: 1; min-height: 0; touch-action: none; cursor: grab; }
canvas:active { cursor: grabbing; }
.legend { display: flex; justify-content: center; flex-wrap: wrap; gap: 12px; font-size: 11px; }
.legend span::before { content: ''; display: inline-block; width: 8px; height: 8px; margin-right: 5px; border-radius: 2px; }
.cutting::before { background: #969692; }.datum::before { background: #377ef2; }.ruby::before { background: #d32643; }
.foot { text-align: center; margin: 9px 8px 12px; font-size: 10px; }
.error { position: absolute; inset: 45% 16px auto; text-align: center; }
</style>
