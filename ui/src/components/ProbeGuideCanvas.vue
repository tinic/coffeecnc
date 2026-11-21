<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref, useTemplateRef, watch } from 'vue'
import {
  AmbientLight, AxesHelper, BoxGeometry, BufferGeometry, CylinderGeometry, DirectionalLight,
  ExtrudeGeometry, Float32BufferAttribute, GridHelper, Group, Line, LineBasicMaterial, Mesh,
  MeshStandardMaterial, OrthographicCamera, Path, Scene, Shape, SphereGeometry, Vector3, WebGLRenderer,
} from 'three'
import { theme } from '../appearance'
import { syncModelCamera } from '../model-camera'
import { orbit, type Camera } from '../pathview'
import type { ProbeGuidePlan, ProbeGuideStep } from '../probe-guide'

const props = defineProps<{ plan: ProbeGuidePlan; playing: boolean }>()
const emit = defineEmits<{ step: [index: number]; unavailable: [] }>()
const canvas = useTemplateRef<HTMLCanvasElement>('canvas')
const failed = ref(false)
const scene = new Scene()
const stage = new Group()
const tool = new Group()
const camera = new OrthographicCamera()
const view: Camera = { azimuth: -0.88, elevation: 0.47, scale: 1, centre: [0, 0, 7] }
const grid = new GridHelper(108, 18, 0x687787, 0x47535f)
const stockMaterial = new MeshStandardMaterial({ color: 0x8195a5, metalness: 0.22, roughness: 0.62 })
const steel = new MeshStandardMaterial({ color: 0xb5c3ce, metalness: 0.57, roughness: 0.33 })
const ruby = new MeshStandardMaterial({ color: 0xcf173d, metalness: 0.16, roughness: 0.25 })
const gold = new MeshStandardMaterial({ color: 0xeeab34, metalness: 0.25, roughness: 0.48 })
const contactMaterial = new MeshStandardMaterial({ color: 0xffb432, emissive: 0xb25514, emissiveIntensity: 0.65 })
const datumMaterial = new MeshStandardMaterial({ color: 0x3addbc, emissive: 0x19745e, emissiveIntensity: 0.7 })
const contact = new Mesh(new SphereGeometry(1.1, 16, 12), contactMaterial)
const datum = new Mesh(new SphereGeometry(1, 16, 12), datumMaterial)
const start = new Mesh(new SphereGeometry(0.7, 14, 10), datumMaterial)
const track = new BufferGeometry()
const trackPoints = new Float32BufferAttribute(new Float32Array(6), 3)
track.setAttribute('position', trackPoints)
const trace = new Line(track, new LineBasicMaterial({ color: 0x44aefe, transparent: true, opacity: 0.92 }))
trace.frustumCulled = false
let stock: Mesh | null = null
let renderer: WebGLRenderer | null = null
let resize: ResizeObserver | null = null
let frame = 0
let previousFrame = 0
let phaseIndex = 0
let phaseTime = 0
let drag: { id: number; x: number; y: number } | null = null

function cylinder(radius: number, height: number, color: MeshStandardMaterial, z: number): Mesh {
  const mesh = new Mesh(new CylinderGeometry(radius, radius, height, 48), color)
  mesh.rotation.x = Math.PI / 2
  mesh.position.z = z
  return mesh
}

function makeStock(): Mesh {
  switch (props.plan.object) {
    case 'bore': {
      const shape = new Shape().moveTo(-24, -18).lineTo(24, -18).lineTo(24, 18).lineTo(-24, 18).closePath()
      shape.holes.push(new Path().absarc(0, 0, 13, 0, 2 * Math.PI, true))
      const mesh = new Mesh(new ExtrudeGeometry(shape, { depth: 14, bevelEnabled: false, curveSegments: 32 }), stockMaterial)
      mesh.position.z = -14
      return mesh
    }
    case 'boss': return cylinder(14, 14, stockMaterial, -7)
    case 'setter': return cylinder(10, 12, gold, -6)
    case 'stock':
    case 'gauge': {
      const mesh = new Mesh(new BoxGeometry(props.plan.object === 'gauge' ? 28 : 48, 36, 14), stockMaterial)
      mesh.position.z = -7
      return mesh
    }
  }
}

function build(): void {
  if (stock !== null) {
    stage.remove(stock)
    stock.geometry.dispose()
  }
  stock = makeStock()
  stage.add(stock)
  if (props.plan.steps[0]) start.position.set(...props.plan.steps[0].point)
  datum.position.set(...props.plan.datum)
  const [x, y] = props.plan.datum
  view.azimuth = x === 0 && y === 0 ? -2.3
    : Math.atan2(x || -16, -(y || -16))
  seek(0)
  setView()
}

function duration(step: ProbeGuideStep): number {
  if (step.kind === 'start') return 1200
  if (step.kind === 'result') return 2100
  return step.kind === 'search' || step.kind === 'contact' ? 960 : 530
}

function draw(): void {
  if (renderer === null || failed.value) return
  const step = props.plan.steps[phaseIndex]
  if (!step) return
  const before = props.plan.steps[phaseIndex - 1] ?? step
  const motion = Math.min(1, phaseTime / (duration(step) * (step.contact ? 0.74 : 0.9)))
  const blend = motion * motion * (3 - 2 * motion)
  const from = new Vector3(...before.point)
  tool.position.copy(from).lerp(new Vector3(...step.point), blend)
  trackPoints.setXYZ(0, ...before.point)
  trackPoints.setXYZ(1, tool.position.x, tool.position.y, tool.position.z)
  trackPoints.needsUpdate = true
  trace.visible = step.kind === 'search' || step.kind === 'move'
  contact.visible = step.contact !== undefined && step.kind !== 'result' && motion >= 1
  if (step.contact) contact.position.set(...step.contact)
  datum.visible = step.kind === 'result'
  start.visible = step.kind === 'start'
  renderer.render(scene, camera)
}

function seek(index: number): void {
  phaseIndex = index
  const step = props.plan.steps[index]
  phaseTime = !props.playing && step ? duration(step) : 0
  emit('step', phaseIndex)
  draw()
}
defineExpose({ seek })

function animate(now: number): void {
  const delta = previousFrame === 0 ? 0 : Math.min(now - previousFrame, 65)
  previousFrame = now
  if (props.playing && renderer !== null && !failed.value) {
    phaseTime += delta
    const step = props.plan.steps[phaseIndex]
    if (step && phaseTime >= duration(step)) seek((phaseIndex + 1) % props.plan.steps.length)
    draw()
  }
  frame = requestAnimationFrame(animate)
}

function setView(): void {
  const surface = canvas.value
  if (renderer === null || surface === null) return
  const width = surface.clientWidth
  const height = surface.clientHeight
  if (width <= 0 || height <= 0) return
  view.scale = Math.min(width / 112, height / 77)
  syncModelCamera(camera, view, width, height, 110)
  renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2))
  renderer.setSize(width, height, false)
  draw()
}

function applyTheme(): void {
  stockMaterial.color.setHex(theme.value === 'dark' ? 0x7895ab : 0x728b9e)
  draw()
}

function down(event: PointerEvent): void {
  if (event.button !== 0 || failed.value) return
  canvas.value?.setPointerCapture(event.pointerId)
  drag = { id: event.pointerId, x: event.clientX, y: event.clientY }
}

function move(event: PointerEvent): void {
  if (drag?.id !== event.pointerId) return
  Object.assign(view, orbit(view, event.clientX - drag.x, event.clientY - drag.y))
  drag.x = event.clientX
  drag.y = event.clientY
  setView()
}

function lost(event: Event): void {
  event.preventDefault()
  failed.value = true
  emit('unavailable')
}

watch(() => props.plan, build)
watch(theme, applyTheme)
onMounted(() => {
  const surface = canvas.value
  if (surface === null) return
  try {
    renderer = new WebGLRenderer({ canvas: surface, alpha: true, antialias: true })
    renderer.setClearColor(0, 0)
    const light = new DirectionalLight(0xffeed5, 2.5)
    light.position.set(-45, -64, 82)
    const rim = new DirectionalLight(0xaacfff, 1.45)
    rim.position.set(50, 44, 25)
    grid.rotation.x = Math.PI / 2
    grid.position.z = -14.1
    const axes = new AxesHelper(10)
    axes.position.set(-30, -23, -13.9)
    tool.add(new Mesh(new SphereGeometry(2, 24, 18), ruby), cylinder(0.8, 9, steel, 6.5),
      cylinder(2.7, 9, steel, 15.5), cylinder(3.6, 5, steel, 21.5))
    scene.add(new AmbientLight(0xffffff, 1.65), light, rim, grid, axes, stage, tool, contact, datum, start, trace)
    build()
    surface.addEventListener('webglcontextlost', lost)
    resize = new ResizeObserver(setView)
    resize.observe(surface)
    applyTheme()
    setView()
    frame = requestAnimationFrame(animate)
  } catch {
    failed.value = true
    emit('unavailable')
  }
})
onBeforeUnmount(() => {
  cancelAnimationFrame(frame)
  resize?.disconnect()
  canvas.value?.removeEventListener('webglcontextlost', lost)
  scene.traverse(node => {
    if (node instanceof Mesh || node instanceof Line) {
      const geometry: unknown = node.geometry
      if (geometry instanceof BufferGeometry) geometry.dispose()
    }
  })
  for (const material of [stockMaterial, steel, ruby, gold, contactMaterial, datumMaterial, trace.material]) material.dispose()
  grid.dispose()
  renderer?.dispose()
})
</script>

<template>
  <div class="guide-canvas">
    <canvas
      ref="canvas" aria-label="3D probe motion: drag to rotate"
      @pointerdown="down" @pointermove="move" @pointerup="drag = null" @pointercancel="drag = null" @lostpointercapture="drag = null"
    ></canvas>
    <p v-if="failed" class="unavailable">3D preview unavailable. Follow the jog and cycle instructions.</p>
  </div>
</template>

<style scoped>
.guide-canvas { position: relative; min-height: 0; min-width: 0; overflow: hidden; background: radial-gradient(ellipse at 50% 46%, var(--panel-raised), var(--bg-sunken) 74%); }
canvas { display: block; width: 100%; height: 100%; cursor: grab; touch-action: none; }
canvas:active { cursor: grabbing; }
.unavailable { position: absolute; inset: 0; display: grid; place-items: center; margin: 0; padding: 30px; color: var(--warn); text-align: center; }
</style>
