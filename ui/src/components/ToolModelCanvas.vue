<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref, useTemplateRef, watch } from 'vue'
import { AmbientLight, DirectionalLight, OrthographicCamera, Scene, WebGLRenderer } from 'three'
import { theme } from '../appearance'
import { syncModelCamera } from '../model-camera'
import { orbit, type Camera } from '../pathview'
import { toolEnvelope, type ToolModel } from '../tool-model'
import { ToolMesh } from '../tool-mesh'

const props = defineProps<{ model: ToolModel }>()
const canvas = useTemplateRef<HTMLCanvasElement>('canvas')
const error = ref<string>('')
const scene = new Scene()
const toolMesh = new ToolMesh()
const camera = new OrthographicCamera()
let renderer: WebGLRenderer | null = null
let resize: ResizeObserver | null = null
let view: Camera = { azimuth: -0.6, elevation: 0.3, scale: 1, centre: [0, 0, 0] }
let drag: { id: number; x: number; y: number } | null = null
let contextLost = false

function build(): void { toolMesh.build(props.model); draw() }
function draw(): void {
  if (renderer === null || canvas.value === null || contextLost) return
  const width = canvas.value.clientWidth; const height = canvas.value.clientHeight
  if (width <= 0 || height <= 0) return
  const { diameter, overallLength } = toolEnvelope(props.model.dimensions)
  const extent = Math.hypot(overallLength, diameter * 1.4)
  view = { ...view, centre: [0, 0, overallLength / 2], scale: Math.max(0.001, Math.min((width - 24) / extent, (height - 24) / extent)) }
  syncModelCamera(camera, view, width, height, extent)
  toolMesh.setTheme(theme.value === 'dark')
  renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2))
  renderer.setSize(width, height, false)
  renderer.render(scene, camera)
}
function down(event: PointerEvent): void {
  if (event.button !== 0) return
  canvas.value?.setPointerCapture(event.pointerId)
  drag = { id: event.pointerId, x: event.clientX, y: event.clientY }
}
function move(event: PointerEvent): void {
  if (drag?.id !== event.pointerId) return
  view = orbit(view, event.clientX - drag.x, event.clientY - drag.y)
  drag.x = event.clientX; drag.y = event.clientY
  draw()
}
function lost(event: Event): void { event.preventDefault(); contextLost = true; error.value = 'Graphics context lost. Dimensions remain editable.' }
function restored(): void { contextLost = false; error.value = ''; draw() }
watch(() => props.model, build, { deep: true })
watch(theme, draw)
onMounted(() => {
  if (canvas.value === null) return
  try {
    renderer = new WebGLRenderer({ canvas: canvas.value, alpha: true, antialias: true })
    renderer.setClearColor(0, 0)
    scene.add(toolMesh.group, new AmbientLight(0xffffff, 1.8))
    const key = new DirectionalLight(0xffffff, 3)
    key.position.set(-80, -120, 180)
    scene.add(key)
    canvas.value.addEventListener('webglcontextlost', lost)
    canvas.value.addEventListener('webglcontextrestored', restored)
    resize = new ResizeObserver(draw)
    resize.observe(canvas.value)
    build()
  } catch { error.value = '3D preview needs WebGL 2. Dimensions remain editable.' }
})
onBeforeUnmount(() => {
  resize?.disconnect()
  canvas.value?.removeEventListener('webglcontextlost', lost)
  canvas.value?.removeEventListener('webglcontextrestored', restored)
  toolMesh.dispose()
  renderer?.dispose(); renderer?.forceContextLoss()
})
</script>

<template>
  <div class="model-canvas">
    <span class="caption label">Envelope preview <span class="hint">· drag to orbit</span></span>
    <canvas ref="canvas" aria-label="Simplified 3D tool envelope" @pointerdown="down" @pointermove="move" @pointerup="drag = null" @lostpointercapture="drag = null" @pointercancel="drag = null"></canvas>
    <p v-if="error" class="error blocked">{{ error }}</p>
    <div v-if="model.family === 'touch-probe'" class="legend"><span class="ruby">Ruby ball</span></div>
    <div v-else class="legend"><span class="cutting">Cutting section</span><span class="datum">Holder-face datum</span></div>
    <p class="hint foot">{{ model.family === 'touch-probe' ? 'Ball bottom at Z = 0' : 'Tip at Z = 0 · body along +Z · holder not modeled' }}</p>
  </div>
</template>

<style scoped>
.model-canvas { position: relative; display: flex; flex-direction: column; min-height: 0; height: 100%; border: 1px solid var(--line); border-radius: 8px; background: var(--bg-sunken); overflow: hidden; }
.caption { padding: 12px 12px 0; pointer-events: none; }
canvas { width: 100%; flex: 1; min-height: 0; touch-action: none; cursor: grab; }
canvas:active { cursor: grabbing; }
.legend { display: flex; justify-content: center; flex-wrap: wrap; gap: 12px; font-size: 11px; }
.legend span::before { content: ''; display: inline-block; width: 8px; height: 8px; margin-right: 5px; border-radius: 2px; }
.cutting::before { background: #bc9954; }.datum::before { background: #377ef2; }.ruby::before { background: #d32643; }
.foot { text-align: center; margin: 9px 8px 12px; font-size: 10px; }
.error { position: absolute; inset: 45% 16px auto; text-align: center; }
</style>
