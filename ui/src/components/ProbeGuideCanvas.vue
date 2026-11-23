<script setup lang="ts">
import { onBeforeUnmount, onMounted, ref, useTemplateRef, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import {
  Box3, BufferGeometry, ExtrudeGeometry, Float32BufferAttribute, GridHelper, Group,
  LatheGeometry, Line, LineBasicMaterial, Mesh,
  MeshBasicMaterial, MeshStandardMaterial, OrthographicCamera, Path, PCFShadowMap,
  PlaneGeometry, RingGeometry, Scene, ShadowMaterial, Shape, Vector2, Vector3, WebGLRenderer,
} from 'three'
import { theme } from '../appearance'
import { isDarkTheme } from '../operator-settings'
import { brushedFinish } from '../brushed-finish'
import { syncModelCamera } from '../model-camera'
import { basis, orbit, type Camera } from '../pathview'
import { lightPreview, PreviewReflections } from '../preview-lighting'
import type { ProbeGuidePlan, ProbeGuideStep } from '../probe-guide'
import { ToolMesh } from '../tool-mesh'

const props = defineProps<{ plan: ProbeGuidePlan; playing: boolean; ruby: boolean }>()
const emit = defineEmits<{ step: [index: number]; unavailable: [] }>()
const { t } = useI18n()
const canvas = useTemplateRef<HTMLCanvasElement>('canvas')
const failed = ref(false)
const scene = new Scene()
const reflections = new PreviewReflections()
const stage = new Group()
const tool = new Group()
const toolMesh = new ToolMesh()
const camera = new OrthographicCamera()
const view: Camera = { azimuth: -0.88, elevation: 0.60, scale: 1, centre: [0, 0, 9] }
const grid = new GridHelper(132, 22, 0x7490a5, 0x7490a5)
const floor = new Mesh(new PlaneGeometry(240, 240), new ShadowMaterial({ opacity: 0.2 }))
const stockMaterial = new MeshStandardMaterial({ metalness: 0.61, roughness: 0.36 })
const machinedTop = stockMaterial.clone()
const walls = new MeshStandardMaterial({ color: 0x8098aa, metalness: 0.54, roughness: 0.4 })
const baseMaterial = new MeshStandardMaterial({ color: 0x394653, metalness: 0.5, roughness: 0.42 })
const plunger = new MeshStandardMaterial({ color: 0xedb160, metalness: 0.55, roughness: 0.32 })
const finish = brushedFinish()
machinedTop.map = finish

function marker(color: number, radius: number): Mesh<RingGeometry, MeshBasicMaterial> {
  const mesh = new Mesh(new RingGeometry(radius * 0.71, radius, 40),
    new MeshBasicMaterial({ color, transparent: true, opacity: 0.95, depthTest: false, depthWrite: false }))
  mesh.renderOrder = 3
  return mesh
}

const contact = marker(0xffb74a, 2.4)
const datum = marker(0x28dec0, 2.55)
const start = marker(0x6fbaff, 1.9)
const track = new BufferGeometry()
const trackPoints = new Float32BufferAttribute(new Float32Array(6), 3)
track.setAttribute('position', trackPoints)
const trace = new Line(track, new LineBasicMaterial({ color: 0x44aefe, transparent: true, opacity: 0.92 }))
trace.frustumCulled = false
let renderer: WebGLRenderer | null = null
let key: ReturnType<typeof lightPreview> | null = null
let resize: ResizeObserver | null = null
let frame = 0
let previousFrame = 0
let phaseIndex = 0
let phaseTime = 0
let drag: { id: number; x: number; y: number } | null = null
let measuredEdge: [Vector3, Vector3] | null = null
let framing: Vector3[] = []

function corners({ min, max }: Box3): Vector3[] {
  return [min.x, max.x].flatMap(x => [min.y, max.y].flatMap(y => [min.z, max.z].map(z => new Vector3(x, y, z))))
}

function turned(profile: readonly [number, number][], material: MeshStandardMaterial): Mesh {
  const geometry = new LatheGeometry(profile.map(([radius, z]) => new Vector2(radius, z)), 64)
  const mesh = new Mesh(geometry.rotateX(Math.PI / 2), material)
  mesh.castShadow = true
  mesh.receiveShadow = true
  return mesh
}

function makeStock(): void {
  switch (props.plan.object) {
    case 'boss':
      stage.add(turned([[0, -14], [13.6, -14], [14, -13.6], [14, -0.55], [13.45, 0], [0, 0]], stockMaterial))
      break
    case 'setter':
      stage.add(turned([[0, -14], [12, -14], [12.8, -13], [12.8, -5], [12, -4], [0, -4]], baseMaterial),
        turned([[0, -4], [8, -4], [9.5, -2.5], [9.5, -0.7], [8.8, 0], [0, 0]], plunger))
      break
    case 'bore':
    case 'stock':
    case 'gauge':
    case 'pocket':
    case 'slot':
    case 'ridge': {
      const bevel = 0.65
      const feature = props.plan.featureHalf
      const ridge = props.plan.object === 'ridge'
      const x = (ridge && feature ? feature[0] : props.plan.object === 'gauge' ? 14 : 24) - bevel
      const y = (ridge && feature ? feature[1] : 18) - bevel
      const shape = new Shape().moveTo(-x, -y).lineTo(x, -y).lineTo(x, y).lineTo(-x, y).closePath()
      if (props.plan.object === 'bore') shape.holes.push(new Path().absarc(0, 0, 13, 0, 2 * Math.PI, true))
      if (feature && !ridge) {
        const [hx, hy] = feature.map(value => value + bevel) as [number, number]
        shape.holes.push(new Path().moveTo(-hx, -hy).lineTo(-hx, hy).lineTo(hx, hy).lineTo(hx, -hy).closePath())
        const bottom = new Mesh(new PlaneGeometry(hx * 2, hy * 2), walls)
        bottom.position.z = -10
        bottom.receiveShadow = true
        stage.add(bottom)
      }
      const geometry = new ExtrudeGeometry(shape, { depth: 14 - 2 * bevel, bevelThickness: bevel,
        bevelSize: bevel, bevelSegments: 2, curveSegments: 48, steps: 1 })
      const stock = new Mesh(geometry, [machinedTop, walls])
      stock.position.z = -14 + bevel
      stock.castShadow = true
      stock.receiveShadow = true
      stage.add(stock)
      break
    }
  }
}

function clearStock(): void {
  for (const part of stage.children) {
    if (part instanceof Mesh) {
      const geometry: unknown = part.geometry
      if (geometry instanceof BufferGeometry) geometry.dispose()
    }
  }
  stage.clear()
}

function build(): void {
  clearStock()
  makeStock()
  stage.rotation.z = props.plan.rotation
  view.elevation = ['bore', 'pocket', 'slot'].includes(props.plan.object) ? 0.87 : 0.57
  const toolBounds = corners(new Box3().setFromObject(toolMesh.group)).map(point => point.sub(tool.position))
  framing = corners(new Box3().setFromObject(stage))
  for (const step of props.plan.steps) {
    const position = new Vector3(...step.point)
    framing.push(...toolBounds.map(point => point.clone().add(position)))
  }
  if (props.plan.steps[0]) start.position.set(...props.plan.steps[0].point)
  datum.position.set(...props.plan.datum)
  const [x, y] = props.plan.datum
  const faces = props.plan.steps.flatMap(step => step.kind === 'search' && step.contact ? [step.contact] : [])
  const [first, second] = faces
  measuredEdge = props.plan.rotation && first && second ? [new Vector3(...first), new Vector3(...second)] : null
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
  const showEdge = step.kind === 'result' && measuredEdge !== null
  if (showEdge && measuredEdge) {
    const [a, b] = measuredEdge
    trackPoints.setXYZ(0, a.x, a.y, a.z)
    trackPoints.setXYZ(1, b.x, b.y, b.z)
  }
  trackPoints.needsUpdate = true
  trace.visible = showEdge || step.kind === 'search' || step.kind === 'move'
  contact.visible = step.contact !== undefined && step.kind !== 'result' && motion >= 1
  if (step.contact) contact.position.set(...step.contact)
  datum.visible = step.kind === 'result'
  start.visible = step.kind === 'start'
  for (const cue of [contact, datum, start]) cue.quaternion.copy(camera.quaternion)
  renderer.render(scene, camera)
}

function seek(index: number, settle = !props.playing): void {
  phaseIndex = index
  const step = props.plan.steps[index]
  phaseTime = settle && step ? duration(step) : 0
  emit('step', phaseIndex)
  draw()
}
defineExpose({ seek })

// Frames are asked for only while the guide plays; paused, it draws on a change only.
function animate(now: number): void {
  frame = 0
  if (!props.playing || renderer === null || failed.value) return
  const delta = previousFrame === 0 ? 0 : Math.min(now - previousFrame, 65)
  previousFrame = now
  phaseTime += delta
  const step = props.plan.steps[phaseIndex]
  if (step && phaseTime >= duration(step)) seek((phaseIndex + 1) % props.plan.steps.length)
  draw()
  frame = requestAnimationFrame(animate)
}

function play(): void {
  if (frame !== 0 || !props.playing || renderer === null || failed.value) return
  previousFrame = 0
  frame = requestAnimationFrame(animate)
}

function setView(): void {
  const surface = canvas.value
  if (renderer === null || surface === null) return
  const width = surface.clientWidth
  const height = surface.clientHeight
  if (width <= 48 || height <= 40) return
  const { right, up } = basis(view)
  const horizontal = new Vector3(...right)
  const vertical = new Vector3(...up)
  const x = framing.map(point => point.dot(horizontal))
  const y = framing.map(point => point.dot(vertical))
  const left = Math.min(...x), rightEdge = Math.max(...x)
  const bottom = Math.min(...y), top = Math.max(...y)
  view.scale = Math.min((width - 48) / (rightEdge - left), (height - 40) / (top - bottom), 4.2)
  view.centre = horizontal.multiplyScalar((left + rightEdge) / 2).addScaledVector(vertical, (bottom + top) / 2).toArray()
  syncModelCamera(camera, view, width, height, 110)
  renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2))
  renderer.setSize(width, height, false)
  draw()
}

function applyTheme(): void {
  const dark = isDarkTheme(theme.value)
  stockMaterial.color.setHex(dark ? 0xaeb8c1 : 0xabb5bc)
  machinedTop.color.copy(stockMaterial.color)
  walls.color.setHex(dark ? 0x919fa9 : 0x929fa7)
  grid.material.opacity = dark ? 0.095 : 0.10
  contact.material.color.setHex(dark ? 0xffbc57 : 0xc66a14)
  datum.material.color.setHex(dark ? 0x3edac5 : 0x007b68)
  start.material.color.setHex(dark ? 0x6fbaff : 0x216ece)
  toolMesh.setTheme(dark)
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
watch(() => props.playing, play)
onMounted(() => {
  const surface = canvas.value
  if (surface === null) return
  try {
    renderer = new WebGLRenderer({ canvas: surface, alpha: true, antialias: true })
    key = lightPreview(scene, renderer)
    reflections.build(scene, renderer)
    renderer.shadowMap.enabled = true
    renderer.shadowMap.type = PCFShadowMap
    key.castShadow = true
    key.shadow.camera.left = key.shadow.camera.bottom = -57
    key.shadow.camera.right = key.shadow.camera.top = 57
    key.shadow.camera.far = 160
    key.shadow.normalBias = 0.12
    key.shadow.radius = 3
    key.shadow.mapSize.set(512, 512)
    grid.rotation.x = Math.PI / 2
    grid.position.z = -14.1
    grid.material.transparent = true
    grid.material.depthWrite = false
    floor.position.z = -14.2
    floor.receiveShadow = true
    toolMesh.build(props.ruby
      ? { family: 'touch-probe', dimensions: { diameter: 4, bodyDiameter: 10, bodyLength: 10,
        stylusLength: 10, shankDiameter: 5.5 }, nominalDiameter: false, customized: false }
      : { family: 'end-mill', dimensions: { diameter: 5, cuttingLength: 10, shankDiameter: 5,
        overallLength: 24, stickout: 19, tipAngle: 90 }, nominalDiameter: false, customized: false })
    toolMesh.group.position.z = -2
    toolMesh.group.traverse(node => {
      if (node instanceof Mesh) node.castShadow = true
      else if (node instanceof Line) node.visible = false
    })
    tool.add(toolMesh.group)
    scene.add(floor, grid, stage, tool, contact, datum, start, trace)
    build()
    surface.addEventListener('webglcontextlost', lost)
    resize = new ResizeObserver(setView)
    resize.observe(surface)
    applyTheme()
    setView()
    play()
  } catch {
    failed.value = true
    emit('unavailable')
  }
})
onBeforeUnmount(() => {
  cancelAnimationFrame(frame)
  resize?.disconnect()
  canvas.value?.removeEventListener('webglcontextlost', lost)
  clearStock()
  toolMesh.dispose()
  for (const cue of [contact, datum, start]) {
    cue.geometry.dispose()
    cue.material.dispose()
  }
  for (const material of [stockMaterial, machinedTop, walls, baseMaterial, plunger, floor.material, trace.material]) material.dispose()
  finish.dispose()
  floor.geometry.dispose()
  track.dispose()
  grid.dispose()
  key?.shadow.dispose()
  reflections.dispose(scene)
  renderer?.dispose()
  renderer?.forceContextLoss()
})
</script>

<template>
  <div class="guide-canvas">
    <canvas
      ref="canvas" :aria-label="t('probe.guideCanvas.label')"
      @pointerdown="down" @pointermove="move" @pointerup="drag = null" @pointercancel="drag = null" @lostpointercapture="drag = null"
    ></canvas>
    <p v-if="failed" class="unavailable">{{ t('probe.guideCanvas.unavailable') }}</p>
  </div>
</template>

<style scoped>
.guide-canvas { position: relative; min-height: 0; min-width: 0; overflow: hidden; background: var(--preview-backdrop); }
canvas { display: block; width: 100%; height: 100%; cursor: grab; touch-action: none; }
canvas:active { cursor: grabbing; }
.unavailable { position: absolute; inset: 0; display: grid; place-items: center; margin: 0; padding: 30px; color: var(--warn); text-align: center; }
</style>
