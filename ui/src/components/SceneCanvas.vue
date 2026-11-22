<script setup lang="ts">
import { onBeforeUnmount, onMounted, useTemplateRef, watch } from 'vue'
import {
  BufferAttribute, BufferGeometry, Color, EdgesGeometry, Group,
  LineBasicMaterial, LineSegments, Matrix4, Mesh, MeshStandardMaterial, OrthographicCamera, Scene, WebGLRenderer,
} from 'three'
import { theme } from '../appearance'
import { syncModelCamera } from '../model-camera'
import { boxCorners, type Camera } from '../pathview'
import { lightPreview, PreviewReflections } from '../preview-lighting'
import { part, showPart } from '../step'
import { modelToMachine } from '../step-model'
import { ToolMesh } from '../tool-mesh'
import { toolPreviewBounds, type ToolPreview } from '../tool-preview'
import { probeFixtureBounds } from '../sim/probe-fixture'
import { buildProbeFixture } from '../sim/probe-mesh'

const props = defineProps<{ camera: Camera; width: number; height: number; tool: ToolPreview | null; probeFixture: boolean }>()
const emit = defineEmits<{ error: [message: string] }>()
const canvas = useTemplateRef<HTMLCanvasElement>('canvas')
const scene = new Scene()
const reflections = new PreviewReflections()
const modelCamera = new OrthographicCamera()
const group = new Group()
const toolMesh = new ToolMesh()
const geometries: BufferGeometry[] = []
const surfaces: MeshStandardMaterial[] = []
const lines: LineBasicMaterial[] = []
let renderer: WebGLRenderer | null = null
let contextLost = false
let fixture: ReturnType<typeof buildProbeFixture> | null = null

function buildFixture(): void {
  if (fixture) {
    scene.remove(fixture.group)
    fixture.dispose()
  }
  fixture = props.probeFixture ? buildProbeFixture() : null
  if (fixture) scene.add(fixture.group)
  draw()
}

function clear(): void {
  group.clear()
  geometries.splice(0).forEach((geometry) => { geometry.dispose() })
  surfaces.splice(0).forEach((material) => { material.dispose() })
  lines.splice(0).forEach((material) => { material.dispose() })
}

function build(): void {
  clear()
  for (const mesh of part.attachment?.geometry.meshes ?? []) {
    const geometry = new BufferGeometry()
    geometry.setAttribute('position', new BufferAttribute(mesh.positions, 3))
    geometry.setIndex(new BufferAttribute(mesh.indices, 1))
    if (mesh.normals !== null) geometry.setAttribute('normal', new BufferAttribute(mesh.normals, 3))
    else geometry.computeVertexNormals()
    geometries.push(geometry)
    const material = new MeshStandardMaterial({
      color: mesh.color === null ? new Color('#b7bdc2') : new Color(...mesh.color),
      metalness: mesh.color === null ? 0.63 : 0.12,
      roughness: mesh.color === null ? 0.34 : 0.45, transparent: true, opacity: part.opacity,
      polygonOffset: true, polygonOffsetFactor: 1, polygonOffsetUnits: 1,
    })
    surfaces.push(material)
    group.add(new Mesh(geometry, material))
    // Edge extraction is a display convenience; avoid expensive work for large tessellations.
    if ((part.attachment?.geometry.triangles ?? Infinity) <= 50_000) {
      const edges = new EdgesGeometry(geometry, 30)
      const outline = new LineBasicMaterial({ transparent: true, depthWrite: false, toneMapped: false })
      geometries.push(edges)
      lines.push(outline)
      group.add(new LineSegments(edges, outline))
    }
  }
  draw()
}

function buildTool(): void {
  if (props.tool === null) toolMesh.clear()
  else toolMesh.build(props.tool.model)
  draw()
}

function draw(): void {
  if (renderer === null || contextLost || props.width <= 0 || props.height <= 0) return
  const attachment = part.attachment
  const corners: [number, number, number][] = []
  if (fixture) {
    const { min, max } = probeFixtureBounds()
    corners.push(...boxCorners(min, max))
  }
  group.visible = showPart.value && attachment !== null
  if (attachment !== null && group.visible) {
    const toWorld = (p: [number, number, number]): [number, number, number] => modelToMachine(p, part.placement, attachment.binding.frame)
    const o = toWorld([0, 0, 0])
    const x = toWorld([1, 0, 0]); const y = toWorld([0, 1, 0]); const z = toWorld([0, 0, 1])
    group.matrixAutoUpdate = false
    group.matrix.copy(new Matrix4().set(x[0] - o[0], y[0] - o[0], z[0] - o[0], o[0],
      x[1] - o[1], y[1] - o[1], z[1] - o[1], o[1], x[2] - o[2], y[2] - o[2], z[2] - o[2], o[2], 0, 0, 0, 1))
    corners.push(...boxCorners(attachment.geometry.bounds.min, attachment.geometry.bounds.max).map(toWorld))
  }
  toolMesh.group.visible = props.tool !== null
  if (props.tool !== null) {
    toolMesh.group.position.set(...props.tool.tip)
    toolMesh.setTheme(theme.value === 'dark')
    const bounds = toolPreviewBounds(props.tool)
    corners.push(...boxCorners(bounds.min, bounds.max))
  }
  const extent = Math.max(0, ...corners.map((p) => Math.hypot(p[0] - props.camera.centre[0], p[1] - props.camera.centre[1], p[2] - props.camera.centre[2])))
  syncModelCamera(modelCamera, props.camera, props.width, props.height, extent)
  surfaces.forEach((material) => {
    const transparent = part.opacity < 1
    if (material.transparent !== transparent) {
      material.transparent = transparent
      material.needsUpdate = true
    }
    material.opacity = part.opacity
  })
  lines.forEach((material) => {
    material.color.set(theme.value === 'dark' ? '#c9d7e0' : '#33485b')
    material.opacity = part.opacity * 0.34
  })
  const ratio = Math.min(window.devicePixelRatio, 2)
  const surface = renderer.domElement
  if (renderer.getPixelRatio() !== ratio || surface.width !== Math.floor(props.width * ratio)
    || surface.height !== Math.floor(props.height * ratio)) {
    renderer.setPixelRatio(ratio)
    renderer.setSize(props.width, props.height, false)
  }
  renderer.render(scene, modelCamera)
}

function lost(event: Event): void { event.preventDefault(); contextLost = true; emit('error', '3D preview unavailable: graphics context lost') }
function restored(): void {
  contextLost = false
  if (renderer !== null) reflections.build(scene, renderer)
  emit('error', '')
  draw()
}
watch(() => part.attachment?.geometry, build)
watch(() => props.probeFixture, buildFixture)
// Status changes move the existing mesh; only changed dimensions rebuild it.
watch(() => JSON.stringify(props.tool?.model ?? null), buildTool)
watch([(): unknown => props.camera, (): unknown => props.width, (): unknown => props.height, (): unknown => part.placement, (): unknown => part.attachment?.binding,
  (): unknown => part.opacity, (): unknown => props.tool, showPart, theme], draw)

onMounted(() => {
  if (canvas.value === null) return
  try {
    renderer = new WebGLRenderer({ canvas: canvas.value, alpha: true, antialias: true })
    lightPreview(scene, renderer)
    reflections.build(scene, renderer)
    scene.add(group, toolMesh.group)
    canvas.value.addEventListener('webglcontextlost', lost)
    canvas.value.addEventListener('webglcontextrestored', restored)
    emit('error', '')
    buildFixture()
    build()
    buildTool()
  } catch {
    emit('error', '3D preview needs WebGL 2; the toolpath is still available')
  }
})
onBeforeUnmount(() => {
  canvas.value?.removeEventListener('webglcontextlost', lost)
  canvas.value?.removeEventListener('webglcontextrestored', restored)
  clear()
  fixture?.dispose()
  toolMesh.dispose()
  reflections.dispose(scene)
  renderer?.dispose()
  renderer?.forceContextLoss()
})
</script>

<template><canvas ref="canvas" class="scene-canvas" aria-hidden="true"></canvas></template>

<style scoped>
.scene-canvas { position: absolute; inset: 0; width: 100%; height: 100%; pointer-events: none; }
</style>
