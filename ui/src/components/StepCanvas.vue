<script setup lang="ts">
import { onBeforeUnmount, onMounted, useTemplateRef, watch } from 'vue'
import {
  AmbientLight, BufferAttribute, BufferGeometry, Color, DirectionalLight, EdgesGeometry, Group,
  LineBasicMaterial, LineSegments, Matrix4, Mesh, MeshStandardMaterial, OrthographicCamera, Scene, WebGLRenderer,
} from 'three'
import { theme } from '../appearance'
import { syncModelCamera } from '../model-camera'
import { boxCorners, type Camera } from '../pathview'
import { part, showPart } from '../step'
import { modelToMachine } from '../step-model'

const props = defineProps<{ camera: Camera; width: number; height: number }>()
const emit = defineEmits<{ error: [message: string] }>()
const canvas = useTemplateRef<HTMLCanvasElement>('canvas')
const scene = new Scene()
const modelCamera = new OrthographicCamera()
const group = new Group()
const geometries: BufferGeometry[] = []
const surfaces: MeshStandardMaterial[] = []
const lines: LineBasicMaterial[] = []
let renderer: WebGLRenderer | null = null
let contextLost = false

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
      color: mesh.color === null ? new Color('#a5b5be') : new Color(...mesh.color),
      metalness: 0.12, roughness: 0.65, transparent: true, opacity: part.opacity,
      polygonOffset: true, polygonOffsetFactor: 1, polygonOffsetUnits: 1,
    })
    surfaces.push(material)
    group.add(new Mesh(geometry, material))
    // Edge extraction is a display convenience; avoid expensive work for large tessellations.
    if ((part.attachment?.geometry.triangles ?? Infinity) <= 50_000) {
      const edges = new EdgesGeometry(geometry, 30)
      const outline = new LineBasicMaterial({ color: '#526574', transparent: true, opacity: 0.5 })
      geometries.push(edges)
      lines.push(outline)
      group.add(new LineSegments(edges, outline))
    }
  }
  draw()
}

function draw(): void {
  const attachment = part.attachment
  if (renderer === null || contextLost || attachment === null || props.width <= 0 || props.height <= 0) return
  const toWorld = (p: [number, number, number]): [number, number, number] => modelToMachine(p, part.placement, attachment.binding.frame)
  const o = toWorld([0, 0, 0])
  const x = toWorld([1, 0, 0]); const y = toWorld([0, 1, 0]); const z = toWorld([0, 0, 1])
  group.matrixAutoUpdate = false
  group.matrix.copy(new Matrix4().set(x[0] - o[0], y[0] - o[0], z[0] - o[0], o[0],
    x[1] - o[1], y[1] - o[1], z[1] - o[1], o[1], x[2] - o[2], y[2] - o[2], z[2] - o[2], o[2], 0, 0, 0, 1))
  group.visible = showPart.value
  const corners = boxCorners(attachment.geometry.bounds.min, attachment.geometry.bounds.max).map(toWorld)
  const extent = Math.max(...corners.map((p) => Math.hypot(p[0] - props.camera.centre[0], p[1] - props.camera.centre[1], p[2] - props.camera.centre[2])))
  syncModelCamera(modelCamera, props.camera, props.width, props.height, extent)
  surfaces.forEach((material) => { material.opacity = part.opacity })
  lines.forEach((material) => { material.color.set(theme.value === 'dark' ? '#a2b8c8' : '#526574'); material.opacity = part.opacity * 0.75 })
  renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2))
  renderer.setSize(props.width, props.height, false)
  renderer.render(scene, modelCamera)
}

function lost(event: Event): void { event.preventDefault(); contextLost = true; emit('error', 'Part model unavailable: graphics context lost') }
function restored(): void { contextLost = false; emit('error', ''); draw() }
watch(() => part.attachment?.geometry, build)
watch([(): unknown => props.camera, (): unknown => props.width, (): unknown => props.height, (): unknown => part.placement, (): unknown => part.attachment?.binding,
  (): unknown => part.opacity, showPart, theme], draw)

onMounted(() => {
  if (canvas.value === null) return
  try {
    renderer = new WebGLRenderer({ canvas: canvas.value, alpha: true, antialias: true })
    renderer.setClearColor(0, 0)
    scene.add(group, new AmbientLight(0xffffff, 1.6))
    const key = new DirectionalLight(0xffffff, 2.4)
    key.position.set(-100, -200, 350)
    scene.add(key)
    canvas.value.addEventListener('webglcontextlost', lost)
    canvas.value.addEventListener('webglcontextrestored', restored)
    emit('error', '')
    build()
  } catch {
    emit('error', 'Part model needs WebGL 2; the toolpath is still available')
  }
})
onBeforeUnmount(() => {
  canvas.value?.removeEventListener('webglcontextlost', lost)
  canvas.value?.removeEventListener('webglcontextrestored', restored)
  clear()
  renderer?.dispose()
  renderer?.forceContextLoss()
})
</script>

<template><canvas ref="canvas" class="part-canvas" aria-hidden="true"></canvas></template>

<style scoped>
.part-canvas { position: absolute; inset: 0; width: 100%; height: 100%; pointer-events: none; }
</style>
