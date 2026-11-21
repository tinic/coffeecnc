<script setup lang="ts">
import { requiresHoming } from '../machine-capabilities'
import { vTooltip } from '../tooltip'
// The path view: the planned path from the preview, the part of it the machine has executed, the
// tool position the machine reports, the active work origin and the machine travel.
//
// It is a picture only. Dragging, zooming and the view buttons change the camera and nothing
// else; no gesture on it sends a command.
import { computed, defineAsyncComponent, onBeforeUnmount, onMounted, ref, shallowRef, useTemplateRef, watch } from 'vue'

import { live, panel } from '../machine'
import { theme } from '../appearance'
import {
  boxCorners,
  boxEdges,
  fit,
  orbit,
  pan,
  project,
  toolColour,
  views,
  zoom,
  type Camera,
  type Screen,
  type ViewName,
} from '../pathview'
import { useDrawnPreview } from '../drawn-preview'
import { drawPreviewGrid } from '../preview-grid'
import type { Vec3 } from '../protocol'
import AppIcon from './AppIcon.vue'
import { part, partBlock, showPart } from '../step'
import { modelToMachine } from '../step-model'
import { loadedToolPreview, reportedToolTip, toolPreviewBounds } from '../tool-preview'
import { toolModels } from '../tool-models'
import { demoFixture, hasDemoProbeFixture, probeFixtureBounds } from '../sim/probe-fixture'

const modelError = ref('')
const SceneCanvas = defineAsyncComponent({
  loader: () => import('./SceneCanvas.vue'),
  onError: (_error, _retry, fail): void => { modelError.value = '3D renderer could not load; reload the panel'; fail() },
})
const modelPoints = computed((): Vec3[] => {
  const attachment = part.attachment
  if (!showPart.value || attachment === null) return []
  return boxCorners(attachment.geometry.bounds.min, attachment.geometry.bounds.max)
    .map((point) => modelToMachine(point, part.placement, attachment.binding.frame))
})

const canvas = useTemplateRef<HTMLCanvasElement>('canvas')
const grid = useTemplateRef<HTMLCanvasElement>('grid')
const size = ref<{ width: number; height: number }>({ width: 0, height: 0 })
const camera = shallowRef<Camera>({ ...views.iso, scale: 1, centre: [200, 125, -100] })
const view = ref<ViewName | null>('iso')
/** Refit when the path changes, until the operator moves the camera. */
const autoFit = ref<boolean>(true)
const showMachine = ref(false)

const drawn = useDrawnPreview(panel)

watch(() => panel.program?.revision, () => { autoFit.value = true })

const current = computed(
  () => drawn.value !== null && panel.status !== null && drawn.value.context === panel.status.preview_context,
)

const showComputing = ref(false)
watch(
  () => panel.preview?.state === 'pending' ? panel.preview.context : null,
  (context, _previous, onCleanup) => {
    showComputing.value = false
    if (context === null) return
    const timer = window.setTimeout(() => { showComputing.value = true }, 1000)
    onCleanup(() => { window.clearTimeout(timer) })
  },
  { immediate: true },
)

const banner = computed((): { text: string; tone: '' | 'warn' | 'stop' } => {
  const preview = panel.preview
  if (!panel.status?.program.file) return { text: '', tone: '' }
  if (preview === null) {
    return { text: 'No preview available', tone: '' }
  }
  if (preview.context !== panel.status.preview_context) {
    return { text: 'Preview out of date', tone: 'warn' }
  }
  if (preview.state === 'unavailable') {
    return {
      text: preview.error?.text ?? 'Preview unavailable',
      tone: drawn.value === null ? '' : 'warn',
    }
  }
  if (preview.state === 'pending') {
    return { text: showComputing.value ? 'Computing preview' : '', tone: 'warn' }
  }
  if (!current.value) {
    return { text: 'Preview out of date', tone: 'warn' }
  }
  if (preview.state === 'failed') {
    const line = preview.error?.line ?? 0
    const location = Number.isSafeInteger(line) && line > 0 ? ` at line ${String(line)}` : ''
    const path = preview.segments.some((segment) => segment.points.length >= 6) ? 'partial path shown' : 'no path available'
    const detail = preview.error?.text
    return {
      text: `Preview stopped${location}: ${path}${detail ? ` · ${detail}` : ''}`,
      tone: 'stop',
    }
  }
  return { text: '', tone: '' }
})

const toolOffset = computed((): Vec3 => panel.status?.offsets.tool ?? [0, 0, 0])

/** Machine travel as the tool tip can reach it with the tool offset in effect. */
const travelBox = computed((): { min: Vec3; max: Vec3 } | null => {
  const axes = panel.status?.axes
  const [x, y, z] = axes ?? []
  if (x === undefined || y === undefined || z === undefined || x.max_limit <= x.min_limit) {
    return null
  }
  const t = toolOffset.value
  return {
    min: [x.min_limit - t[0], y.min_limit - t[1], z.min_limit - t[2]],
    max: [x.max_limit - t[0], y.max_limit - t[1], z.max_limit - t[2]],
  }
})

const homingRequired = computed(() => requiresHoming(panel.hello))
const toolTip = computed((): Vec3 | null => reportedToolTip(panel.status, homingRequired.value))
const toolPositionHint = computed(() => {
  const s = panel.status
  if (!live.value || s === null || toolTip.value !== null || s.axes.some(axis => axis.homing)) return ''
  return homingRequired.value && s.axes.some(axis => !axis.homed)
    ? 'Home the axes to show the tool position'
    : 'Tool position unavailable'
})
const toolBody = computed(() => loadedToolPreview(panel.status, live.value, panel.toolTable?.tools ?? [], toolModels.assignments,
  toolModels.defaults, homingRequired.value))
const showTool = ref(true)
const visibleTool = computed(() => showTool.value ? toolBody.value : null)
const probeFixture = computed(() => hasDemoProbeFixture(panel.simulated, panel.hello))
const workFrame = computed(() => {
  if (panel.status === null) return null
  const { g5x, g92, rotation_xy: degrees } = panel.status.offsets
  const angle = degrees * Math.PI / 180
  const cos = Math.cos(angle), sin = Math.sin(angle)
  const origin: Vec3 = [g5x[0] + g92[0] * cos - g92[1] * sin,
    g5x[1] + g92[0] * sin + g92[1] * cos, g5x[2] + g92[2]]
  return { origin, angle, cos, sin }
})

const tools = computed((): number[] => {
  const seen: number[] = []
  for (const segment of drawn.value?.segments ?? []) {
    if (segment.kind === 'feed' && !seen.includes(segment.tool)) {
      seen.push(segment.tool)
    }
  }
  return seen
})



// ---- camera -------------------------------------------------------------------------------------

function pathPoints(): Vec3[] {
  const points: Vec3[] = []
  for (const segment of drawn.value?.segments ?? []) {
    const p = segment.points
    for (let i = 0; i + 2 < p.length; i += 3) {
      points.push([p[i] ?? 0, p[i + 1] ?? 0, p[i + 2] ?? 0])
    }
  }
  return points
}

function fitTo(what: 'path' | 'machine'): void {
  const { width, height } = size.value
  // The path is fitted to its feed moves; rapids may come from far away.
  const bounds = drawn.value?.bounds ?? null
  let points = what !== 'path' ? [] : bounds !== null ? boxCorners(bounds.min, bounds.max) : pathPoints()
  if (what === 'path') {
    points = [...points, ...modelPoints.value]
    if (probeFixture.value) {
      const { min, max } = probeFixtureBounds()
      points.push(...boxCorners(min, max))
    }
    if (visibleTool.value !== null) {
      const bounds = toolPreviewBounds(visibleTool.value)
      points.push(...boxCorners(bounds.min, bounds.max))
    }
  }
  if (points.length === 0 && travelBox.value !== null) {
    points = boxCorners(travelBox.value.min, travelBox.value.max)
  }
  camera.value = fit(camera.value, points, width, height)
}

function fitButton(what: 'path' | 'machine'): void {
  autoFit.value = what === 'path'
  showMachine.value = what === 'machine'
  fitTo(what)
}

function setView(name: ViewName): void {
  view.value = name
  camera.value = { ...camera.value, ...views[name] }
  fitTo('path')
}

watch(() => part.attachment?.geometry, () => { autoFit.value = true; modelError.value = '' })
// Live position is not a fit dependency: motion must not make the camera follow the tool.
watch([drawn, size, modelPoints, probeFixture], () => {
  if (autoFit.value) {
    fitTo('path')
  }
})

const pointers = new Map<number, Screen>()
let pinch = 0

function local(event: PointerEvent | WheelEvent): Screen {
  const rect = (event.currentTarget as HTMLElement).getBoundingClientRect()
  return [event.clientX - rect.left, event.clientY - rect.top]
}

function pointerDown(event: PointerEvent): void {
  ;(event.currentTarget as HTMLElement).setPointerCapture(event.pointerId)
  pointers.set(event.pointerId, local(event))
  pinch = 0
}

function pointerMove(event: PointerEvent): void {
  const before = pointers.get(event.pointerId)
  if (before === undefined) {
    return
  }
  const now = local(event)
  pointers.set(event.pointerId, now)
  autoFit.value = false
  const dx = now[0] - before[0]
  const dy = now[1] - before[1]
  if (pointers.size >= 2) {
    // Two fingers: the distance between them zooms, their movement pans.
    const [a, b] = [...pointers.values()]
    if (a === undefined || b === undefined) {
      return
    }
    const distance = Math.hypot(a[0] - b[0], a[1] - b[1])
    const middle: Screen = [(a[0] + b[0]) / 2, (a[1] + b[1]) / 2]
    let next = pan(camera.value, dx / 2, dy / 2)
    if (pinch > 0 && distance > 0) {
      next = zoom(next, distance / pinch, middle, size.value.width, size.value.height)
    }
    pinch = distance
    camera.value = next
  } else if (event.shiftKey || (event.buttons & 6) !== 0) {
    camera.value = pan(camera.value, dx, dy)
  } else {
    view.value = null
    camera.value = orbit(camera.value, dx, dy)
  }
}

function pointerUp(event: PointerEvent): void {
  pointers.delete(event.pointerId)
  pinch = 0
}

function wheel(event: WheelEvent): void {
  autoFit.value = false
  camera.value = zoom(camera.value, Math.exp(-event.deltaY * 0.0015), local(event), size.value.width, size.value.height)
}

// ---- drawing ------------------------------------------------------------------------------------

const colours = {
  light: { travel: '#778a9a', rapid: '#667989', executed: '#064fc4', marker: '#a35910', outdated: '#687782',
    axes: ['#a34040', '#2e7651', '#375eb0'] },
  dark: { travel: '#7f95a7', rapid: '#8b9dae', executed: '#84bbff', marker: '#ffbd61', outdated: '#8996a3',
    axes: ['#f28c87', '#83cea4', '#91b8ff'] },
} as const

function drawGrid(): void {
  const element = grid.value
  const context = element?.getContext('2d')
  const { width, height } = size.value
  if (element === null || !context || width <= 0 || height <= 0) return
  const ratio = Math.min(window.devicePixelRatio, 2)
  const pixelsWide = Math.round(width * ratio), pixelsHigh = Math.round(height * ratio)
  if (element.width !== pixelsWide || element.height !== pixelsHigh) {
    element.width = pixelsWide
    element.height = pixelsHigh
  }
  context.setTransform(ratio, 0, 0, ratio, 0, 0)
  context.clearRect(0, 0, width, height)
  const frame = workFrame.value
  if (frame === null) return
  const plane: Vec3 = [frame.origin[0], frame.origin[1], probeFixture.value
    ? demoFixture.top - demoFixture.stockHeight : frame.origin[2]]
  drawPreviewGrid(context, camera.value, plane, frame.angle, width, height, theme.value === 'dark')
}
watch([camera, size, theme, probeFixture, (): number | undefined => workFrame.value?.angle,
  (): number | undefined => workFrame.value?.origin[0], (): number | undefined => workFrame.value?.origin[1],
  (): number | undefined => workFrame.value?.origin[2]], drawGrid)

function draw(): void {
  const element = canvas.value
  const context = element?.getContext('2d')
  const { width, height } = size.value
  if (element === null || context === null || context === undefined || width === 0 || height === 0) {
    return
  }
  const ratio = window.devicePixelRatio
  if (element.width !== Math.round(width * ratio) || element.height !== Math.round(height * ratio)) {
    element.width = Math.round(width * ratio)
    element.height = Math.round(height * ratio)
  }
  context.setTransform(ratio, 0, 0, ratio, 0, 0)
  context.clearRect(0, 0, width, height)
  context.lineJoin = 'round'
  context.lineCap = 'round'
  const cam = camera.value
  const at = (p: Vec3): Screen => project(cam, p, width, height)
  const status = panel.status
  const palette = colours[theme.value]

  if (probeFixture.value) {
    const [x, y] = at(demoFixture.setter)
    context.fillStyle = theme.value === 'dark' ? '#e5bd70' : '#845614'
    context.font = '600 12px system-ui, sans-serif'
    context.textAlign = 'center'
    context.textBaseline = 'middle'
    context.fillText('Tool setter', x, y + 30)
  }

  const box = travelBox.value
  if (box !== null && showMachine.value) {
    const corners = boxCorners(box.min, box.max).map(at)
    context.strokeStyle = palette.travel
    context.lineWidth = 1
    context.setLineDash([])
    context.beginPath()
    for (const [a, b] of boxEdges) {
      const from = corners[a]
      const to = corners[b]
      if (from !== undefined && to !== undefined) {
        context.moveTo(from[0], from[1])
        context.lineTo(to[0], to[1])
      }
    }
    context.stroke()
  }

  const preview = drawn.value
  if (preview !== null) {
    const fresh = current.value
    const running = fresh && live.value && status !== null && status.interp !== 'idle' && status.mode === 'auto'
    const motionLine = running ? status.program.motion_line : 0
    const reached = running ? motionLine || status.program.current_line : 0
    // Three passes, so that what has been cut lies on top of what is planned.
    for (const pass of ['rapid', 'planned', 'done'] as const) {
      for (const segment of preview.segments) {
        const done = running && segment.line <= reached
        const kind = segment.kind === 'rapid' ? 'rapid' : done ? 'done' : 'planned'
        if (kind !== pass) {
          continue
        }
        const p = segment.points
        context.beginPath()
        for (let i = 0; i + 2 < p.length; i += 3) {
          const [x, y] = at([p[i] ?? 0, p[i + 1] ?? 0, p[i + 2] ?? 0])
          if (i === 0) {
            context.moveTo(x, y)
          } else {
            context.lineTo(x, y)
          }
        }
        const colour = fresh ? toolColour(tools.value.indexOf(segment.tool), theme.value === 'dark') : palette.outdated
        if (kind === 'rapid') {
          context.setLineDash([2, 5])
          context.strokeStyle = fresh ? palette.rapid : palette.outdated
          context.globalAlpha = done ? 0.8 : 0.56
          context.lineWidth = 1
        } else if (kind === 'done') {
          context.setLineDash([])
          context.strokeStyle = segment.line === motionLine ? palette.executed : colour
          context.globalAlpha = 1
          context.lineWidth = 2.6
        } else {
          context.setLineDash([])
          context.strokeStyle = colour
          context.globalAlpha = fresh ? (running ? 0.45 : 0.9) : 0.5
          context.lineWidth = 1.55
        }
        context.stroke()
      }
    }
    context.globalAlpha = 1
    context.setLineDash([])
  }

  const work = workFrame.value
  if (work !== null) {
    const { origin, cos, sin } = work
    const directions: Vec3[] = [
      [cos, sin, 0],
      [-sin, cos, 0],
      [0, 0, 1],
    ]
    const o = at(origin)
    context.lineWidth = 2
    context.font = '600 11px system-ui, sans-serif'
    context.textAlign = 'center'
    context.textBaseline = 'middle'
    directions.forEach((direction, index) => {
      const tip = at([origin[0] + direction[0], origin[1] + direction[1], origin[2] + direction[2]])
      // A fixed length on screen, however far the view is zoomed.
      const length = Math.hypot(tip[0] - o[0], tip[1] - o[1])
      if (length < cam.scale * 0.2) {
        return
      }
      const reach = (34 * (length / cam.scale)) / length
      const end: Screen = [o[0] + (tip[0] - o[0]) * reach, o[1] + (tip[1] - o[1]) * reach]
      const colour = palette.axes[index] ?? palette.rapid
      context.strokeStyle = colour
      context.fillStyle = colour
      context.globalAlpha = live.value ? 1 : 0.4
      context.beginPath()
      context.moveTo(o[0], o[1])
      context.lineTo(end[0], end[1])
      context.stroke()
      context.fillText(['X', 'Y', 'Z'][index] ?? '', o[0] + (end[0] - o[0]) * 1.3, o[1] + (end[1] - o[1]) * 1.3)
    })
    context.globalAlpha = 1
  }

  const tip = toolTip.value
  if (tip !== null) {
    const [x, y] = at(tip)
    const top = at([tip[0], tip[1], tip[2] + 1])
    const length = Math.hypot(top[0] - x, top[1] - y)
    const modelVisible = visibleTool.value !== null && modelError.value === ''
    const colour = live.value ? palette.marker : palette.outdated
    context.strokeStyle = colour
    context.fillStyle = colour
    if (!modelVisible && length > 0.01) {
      // The tool's axis: a fixed length on screen, pointing up the machine's Z.
      context.lineWidth = 3
      context.beginPath()
      context.moveTo(x, y)
      context.lineTo(x + ((top[0] - x) / length) * 30 * (length / cam.scale), y + ((top[1] - y) / length) * 30 * (length / cam.scale))
      context.stroke()
    }
    context.lineWidth = 2
    context.beginPath()
    context.arc(x, y, modelVisible ? 4 : 6, 0, 2 * Math.PI)
    context.stroke()
    context.beginPath()
    if (!modelVisible) {
      context.arc(x, y, 2, 0, 2 * Math.PI)
      context.fill()
    }
  }
}

let frame = 0

function schedule(): void {
  if (frame === 0) {
    frame = window.requestAnimationFrame(() => {
      frame = 0
      draw()
    })
  }
}

watch([camera, size, drawn, current, (): unknown => panel.status, (): unknown => panel.link,
  tools, theme, showMachine, probeFixture, visibleTool, modelError], schedule)

let observer: ResizeObserver | null = null

onMounted(() => {
  const element = canvas.value
  if (element === null) {
    return
  }
  const measure = (): void => {
    size.value = { width: element.clientWidth, height: element.clientHeight }
  }
  observer = new ResizeObserver(measure)
  observer.observe(element)
  measure()
  schedule()
})

onBeforeUnmount(() => {
  observer?.disconnect()
  if (frame !== 0) {
    window.cancelAnimationFrame(frame)
  }
})
</script>

<template>
  <div class="pathview" :class="{ outdated: drawn !== null && !current }">
    <div class="viewport">
      <canvas ref="grid" class="work-grid" aria-hidden="true"></canvas>
      <SceneCanvas
        v-if="part.attachment || visibleTool || probeFixture"
        :camera="camera" :width="size.width" :height="size.height"
        :tool="visibleTool" :probe-fixture="probeFixture" @error="modelError = $event"
      />
      <canvas ref="canvas" class="toolpath-canvas" aria-label="Toolpath, part and loaded tool preview" @pointerdown="pointerDown" @pointermove="pointerMove" @pointerup="pointerUp" @pointercancel="pointerUp" @wheel.prevent="wheel" @contextmenu.prevent></canvas>
      <div v-if="toolPositionHint || modelError || (live && part.attachment && partBlock)" class="banners">
        <span v-if="toolPositionHint" class="warn">{{ toolPositionHint }}</span>
        <span v-if="modelError || (live && part.attachment && partBlock)" class="warn">{{ modelError || partBlock }}</span>
      </div>
      <span v-if="banner.text" class="preview-banner" :class="banner.tone" role="status">{{ banner.text }}</span>
    </div>
    <div class="scene-bottom">
      <div class="legend">
        <span v-if="probeFixture">Simulated fixtures</span>
        <span v-if="!probeFixture || drawn"><i class="feed"></i>Cut</span>
        <span v-if="!probeFixture || drawn"><i class="rapid"></i>Rapid</span>
        <label v-if="toolBody"><input v-model="showTool" type="checkbox" />Tool</label>
      </div>
      <div class="views">
        <button type="button" :aria-pressed="view === 'iso'" @click="setView('iso')">Iso</button>
        <button type="button" :aria-pressed="view === 'top'" @click="setView('top')">Top</button>
        <button type="button" :aria-pressed="view === 'front'" @click="setView('front')">Front</button>
        <button v-tooltip="'Fit toolpath and visible models'" type="button" aria-label="Fit toolpath and visible models" @click="fitButton('path')"><AppIcon name="fit" /></button>
        <button v-tooltip="'Show and fit machine travel'" type="button" @click="fitButton('machine')">Travel</button>
        <slot />
      </div>
    </div>
  </div>
</template>

<style scoped>
.pathview { position: relative; min-width: 0; min-height: 0; overflow: hidden; }
.viewport { position: relative; height: calc(100% - 42px); background: var(--preview-backdrop); }
.work-grid { position: absolute; inset: 0; width: 100%; height: 100%; pointer-events: none; mask-image: radial-gradient(ellipse at center, #000 22%, transparent 74%); }
.toolpath-canvas { position: relative; display: block; width: 100%; height: 100%; cursor: grab; touch-action: none; }
.scene-bottom { display: flex; align-items: center; justify-content: space-between; gap: 12px; height: 42px; padding: 0 18px 8px; color: var(--text-dim); font-size: 10px; }
.legend { display: flex; gap: 14px; }.legend span { display: flex; align-items: center; gap: 6px; }.legend i { width: 17px; height: 2px; background: var(--accent); }.legend .rapid { height: 0; border-top: 1px dashed var(--text-dim); background: none; }
.legend label { display: flex; align-items: center; gap: 4px; cursor: pointer; }
.views { display: flex; align-items: center; gap: 2px; }.views button { padding: 5px 7px; min-height: 30px; border: 0; border-radius: 6px; font-size: 10px; background: none; color: var(--text-dim); cursor: pointer; }.views button[aria-pressed=true] { color: var(--accent); background: var(--panel); }.views :deep(svg) { display: block; width: 14px; height: 14px; }
.banners, .preview-banner { position: absolute; left: 18px; max-width: calc(100% - 36px); pointer-events: none; font-size: 11px; color: var(--text-dim); }
.banners { top: 12px; display: flex; flex-direction: column; align-items: flex-start; gap: 6px; }
.preview-banner { bottom: 12px; }
.banners span, .preview-banner { padding: 5px 8px; border-radius: 6px; background: var(--panel); }
.stop { color: var(--stop); }.warn { color: var(--warn); }
</style>
