<script setup lang="ts">
import { requiresHoming } from '../machine-capabilities'
import { vTooltip } from '../tooltip'
// The path view: the planned path from the preview, the part of it the machine has executed, the
// tool position the machine reports, the active work origin and the machine travel.
//
// It is a picture only. Dragging, zooming and the view buttons change the camera and nothing
// else; no gesture on it sends a command. A tap picks the move under the finger and reports its
// source line, and the moves of highlighted lines are drawn on top: this links it to the text.
import { computed, defineAsyncComponent, type ComputedRef, onBeforeUnmount, onMounted, ref, shallowRef, useTemplateRef, watch } from 'vue'
import { useI18n } from 'vue-i18n'

import { live, panel } from '../machine'
import { theme } from '../appearance'
import { isDarkTheme } from '../operator-settings'
import {
  boxCorners,
  boxEdges,
  fit,
  orbit,
  pan,
  project,
  toolColour,
  travelBounds,
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
import { buildPickIndex, isTap, pickSegment } from '../path-pick'
import { loadedToolPreview, reportedToolTip, toolPreviewBounds } from '../tool-preview'
import { toolModels } from '../tool-models'
import { demoFixture, hasDemoProbeFixture, probeFixtureBounds } from '../sim/probe-fixture'
import { translateMessage } from '../i18n/messages'

const { t } = useI18n()

const props = defineProps<{
  /** Source lines whose moves are drawn highlighted, inclusive; null for none. */
  highlight?: { first: number; last: number } | null
}>()
const emit = defineEmits<{
  /** A tap on the path: the source line of the move under it, or 0 for a tap beside the path. */
  pick: [line: number]
}>()

const modelError = ref('')

/**
 * A computed that keeps its last object while the value stays the same. The status is a new
 * object 20 times a second even at rest, and drawing again for each would redraw the same picture.
 */
function unchanged<T>(get: () => T): ComputedRef<T> {
  return computed((previous) => {
    const next = get()
    return previous !== undefined && JSON.stringify(previous) === JSON.stringify(next) ? previous : next
  })
}
const SceneCanvas = defineAsyncComponent({
  loader: () => import('./SceneCanvas.vue'),
  onError: (_error, _retry, fail): void => { modelError.value = t('program.pathView.rendererFailed'); fail() },
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
    return { text: t('program.pathView.noPreview'), tone: '' }
  }
  if (preview.context !== panel.status.preview_context) {
    return { text: t('program.pathView.outOfDate'), tone: 'warn' }
  }
  if (preview.state === 'unavailable') {
    return {
      text: preview.error ? translateMessage(preview.error.text) : t('program.pathView.unavailable'),
      tone: drawn.value === null ? '' : 'warn',
    }
  }
  if (preview.state === 'pending') {
    return { text: showComputing.value ? t('program.pathView.computing') : '', tone: 'warn' }
  }
  if (!current.value) {
    return { text: t('program.pathView.outOfDate'), tone: 'warn' }
  }
  if (preview.state === 'failed') {
    const line = preview.error?.line ?? 0
    const path = preview.segments.some((segment) => segment.points.length >= 6) ? t('program.pathView.partialPath') : t('program.pathView.noPath')
    const stopped = Number.isSafeInteger(line) && line > 0
      ? t('program.pathView.stoppedAtLine', { line, path }) : t('program.pathView.stopped', { path })
    const detail = preview.error?.text
    return {
      text: detail ? `${stopped} · ${translateMessage(detail)}` : stopped,
      tone: 'stop',
    }
  }
  return { text: '', tone: '' }
})

const toolOffset = computed((): Vec3 => panel.status?.offsets.tool ?? [0, 0, 0])

/** Machine travel as the tool tip can reach it with the tool offset in effect. */
const travelBox = unchanged(() => travelBounds(panel.status?.axes, toolOffset.value))

const homingRequired = computed(() => requiresHoming(panel.hello))
const toolTip = unchanged((): Vec3 | null => reportedToolTip(panel.status, homingRequired.value))
const toolPositionHint = computed(() => {
  const s = panel.status
  if (!live.value || s === null || toolTip.value !== null || s.axes.some(axis => axis.homing)) return ''
  return homingRequired.value && s.axes.some(axis => !axis.homed)
    ? t('program.pathView.homeForTool')
    : t('program.pathView.toolUnavailable')
})
const toolBody = unchanged(() => loadedToolPreview(panel.status, live.value, panel.toolTable?.tools ?? [], toolModels.assignments,
  toolModels.defaults, homingRequired.value))
const showTool = ref(true)
const visibleTool = computed(() => showTool.value ? toolBody.value : null)
const probeFixture = computed(() => hasDemoProbeFixture(panel.simulated, panel.hello))
const workFrame = unchanged(() => {
  if (panel.status === null) return null
  const { g5x, g92, rotation_xy: degrees } = panel.status.offsets
  const angle = degrees * Math.PI / 180
  const cos = Math.cos(angle), sin = Math.sin(angle)
  const origin: Vec3 = [g5x[0] + g92[0] * cos - g92[1] * sin,
    g5x[1] + g92[0] * sin + g92[1] * cos, g5x[2] + g92[2]]
  return { origin, angle, cos, sin }
})

/**
 * How far the running program has come: the drawing's only use of the program's progress. Only
 * the motion line counts; the interpreter's current line reads ahead of what has been cut.
 */
const executed = unchanged(() => {
  const status = panel.status
  const running = current.value && live.value && status !== null && status.interp !== 'idle' && status.mode === 'auto'
  const motionLine = running ? status.program.motion_line : 0
  return { running, motionLine, reached: motionLine }
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
/** The gesture in progress: a tap until the pointer moves past the slop or a second one comes. */
let gesture: { start: Screen; tap: boolean } | null = null
/** Built on the first tap after the path changes; drawing does not need it. */
const pickIndex = computed(() => buildPickIndex(drawn.value?.segments ?? []))

function local(event: PointerEvent | WheelEvent): Screen {
  const rect = (event.currentTarget as HTMLElement).getBoundingClientRect()
  return [event.clientX - rect.left, event.clientY - rect.top]
}

function pointerDown(event: PointerEvent): void {
  ;(event.currentTarget as HTMLElement).setPointerCapture(event.pointerId)
  const at = local(event)
  if (pointers.size === 0) gesture = { start: at, tap: event.button === 0 && !event.shiftKey }
  else if (gesture !== null) gesture.tap = false
  pointers.set(event.pointerId, at)
  pinch = 0
}

function pointerMove(event: PointerEvent): void {
  const before = pointers.get(event.pointerId)
  if (before === undefined) {
    return
  }
  const now = local(event)
  if (gesture?.tap === true && pointers.size === 1) {
    // A finger shifts a little during a tap; only a larger movement starts a drag.
    if (isTap(gesture.start, now)) return
    gesture.tap = false
  }
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
  const tracked = pointers.delete(event.pointerId)
  pinch = 0
  if (pointers.size > 0) return
  const at = local(event)
  // The release point counts too: a pointer can travel without a final pointermove.
  const tap = tracked && event.type === 'pointerup' && gesture?.tap === true && isTap(gesture.start, at)
  gesture = null
  if (tap) emit('pick', pickAt(at))
}

/** The source line of the move nearest to `at` on the screen, or 0. */
function pickAt(at: Screen): number {
  if (drawn.value === null) return 0
  return pickSegment(pickIndex.value, camera.value, at, size.value.width, size.value.height)?.line ?? 0
}

function wheel(event: WheelEvent): void {
  autoFit.value = false
  camera.value = zoom(camera.value, Math.exp(-event.deltaY * 0.0015), local(event), size.value.width, size.value.height)
}

// ---- drawing ------------------------------------------------------------------------------------

const colours = {
  light: { travel: '#778a9a', rapid: '#536879', executed: '#064fc4', marker: '#a35910', outdated: '#687782',
    picked: '#d9480f', halo: '#ffffff', axes: ['#a34040', '#2e7651', '#375eb0'] },
  dark: { travel: '#7f95a7', rapid: '#a2b3c5', executed: '#84bbff', marker: '#ffbd61', outdated: '#8996a3',
    picked: '#ff8a4c', halo: '#1c242d', axes: ['#f28c87', '#83cea4', '#91b8ff'] },
} as const
const legendInk = computed(() => {
  const dark = isDarkTheme(theme.value)
  return { '--cut-sample': toolColour(0, dark), '--rapid-sample': colours[dark ? 'dark' : 'light'].rapid }
})

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
  drawPreviewGrid(context, camera.value, plane, frame.angle, width, height, isDarkTheme(theme.value))
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
  const palette = colours[isDarkTheme(theme.value) ? 'dark' : 'light']

  if (probeFixture.value) {
    const [x, y] = at(demoFixture.setter)
    context.fillStyle = isDarkTheme(theme.value) ? '#e5bd70' : '#845614'
    context.font = '600 12px system-ui, sans-serif' // i18n-ignore
    context.textAlign = 'center'
    context.textBaseline = 'middle'
    context.fillText(t('program.pathView.toolSetter'), x, y + 30)
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
    const { running, motionLine, reached } = executed.value
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
        const colour = fresh ? toolColour(tools.value.indexOf(segment.tool), isDarkTheme(theme.value)) : palette.outdated
        if (kind === 'rapid') {
          context.setLineDash([3, 4])
          context.strokeStyle = fresh ? palette.rapid : palette.outdated
          context.globalAlpha = fresh ? 0.84 : 0.56
          context.lineWidth = 1.25
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
    const highlight = props.highlight
    if (highlight && highlight.first > 0) {
      // The picked lines on top of everything, outlined so they read over the planned path.
      let left = Infinity, top = Infinity, right = -Infinity, bottom = -Infinity
      context.beginPath()
      for (const segment of preview.segments) {
        if (segment.line < highlight.first || segment.line > highlight.last) continue
        const p = segment.points
        for (let i = 0; i + 2 < p.length; i += 3) {
          const [x, y] = at([p[i] ?? 0, p[i + 1] ?? 0, p[i + 2] ?? 0])
          if (i === 0) context.moveTo(x, y)
          else context.lineTo(x, y)
          left = Math.min(left, x)
          top = Math.min(top, y)
          right = Math.max(right, x)
          bottom = Math.max(bottom, y)
        }
      }
      // A short move is a few pixels long: a ring around it shows where it is.
      if (right >= left && Math.hypot(right - left, bottom - top) < 16) {
        context.moveTo((left + right) / 2 + 10, (top + bottom) / 2)
        context.arc((left + right) / 2, (top + bottom) / 2, 10, 0, 2 * Math.PI)
      }
      for (const [width, colour] of [[6, palette.halo], [3, palette.picked]] as const) {
        context.lineWidth = width
        context.strokeStyle = colour
        context.globalAlpha = width === 6 ? 0.85 : 1
        context.stroke()
      }
      context.globalAlpha = 1
    }
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
    context.font = '600 11px system-ui, sans-serif' // i18n-ignore
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

watch([camera, size, drawn, current, executed, travelBox, workFrame, toolTip, live,
  tools, theme, showMachine, probeFixture, visibleTool, modelError,
  (): number => props.highlight?.first ?? 0, (): number => props.highlight?.last ?? 0], schedule)

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
      <canvas ref="canvas" class="toolpath-canvas" :aria-label="t('program.pathView.canvasLabel')" @pointerdown="pointerDown" @pointermove="pointerMove" @pointerup="pointerUp" @pointercancel="pointerUp" @lostpointercapture="pointerUp" @wheel.prevent="wheel" @contextmenu.prevent></canvas>
      <div v-if="toolPositionHint || modelError || (live && part.attachment && partBlock)" class="banners">
        <span v-if="toolPositionHint" class="warn">{{ toolPositionHint }}</span>
        <span v-if="modelError || (live && part.attachment && partBlock)" class="warn">{{ modelError || partBlock }}</span>
      </div>
      <span v-if="banner.text" class="preview-banner" :class="banner.tone" role="status">{{ banner.text }}</span>
    </div>
    <div class="scene-bottom">
      <div class="legend" :style="legendInk">
        <span v-if="probeFixture">{{ t('program.pathView.simulatedFixtures') }}</span>
        <span v-if="!probeFixture || drawn"><i class="feed"></i>{{ t('program.pathView.cut') }}</span>
        <span v-if="!probeFixture || drawn"><i class="rapid"></i>{{ t('program.pathView.rapid') }}</span>
        <label v-if="toolBody"><input v-model="showTool" type="checkbox" />{{ t('program.pathView.tool') }}</label>
      </div>
      <div class="views">
        <button class="tone-control choice" type="button" :aria-pressed="view === 'iso'" @click="setView('iso')">{{ t('program.pathView.iso') }}</button>
        <button class="tone-control choice" type="button" :aria-pressed="view === 'top'" @click="setView('top')">{{ t('program.pathView.top') }}</button>
        <button class="tone-control choice" type="button" :aria-pressed="view === 'front'" @click="setView('front')">{{ t('program.pathView.front') }}</button>
        <button v-tooltip="t('program.pathView.fit')" class="btn square" type="button" :aria-label="t('program.pathView.fit')" @click="fitButton('path')"><AppIcon name="fit" /></button>
        <button v-tooltip="t('program.pathView.travelTooltip')" class="btn travel-control" type="button" @click="fitButton('machine')">{{ t('program.pathView.travel') }}</button>
        <slot />
      </div>
    </div>
  </div>
</template>

<style scoped>
.pathview { position: relative; display: flex; flex-direction: column; min-width: 0; min-height: 0; overflow: hidden; }
.viewport { position: relative; flex: 1; min-height: 0; background: var(--preview-backdrop); }
.work-grid { position: absolute; inset: 0; width: 100%; height: 100%; pointer-events: none; mask-image: radial-gradient(ellipse at center, #000 28%, transparent 83%); }
.toolpath-canvas { position: relative; display: block; width: 100%; height: 100%; cursor: grab; touch-action: none; }
.scene-bottom { display: flex; flex: none; flex-wrap: wrap; align-items: center; justify-content: space-between; gap: 2px 12px; min-height: 52px; padding: 4px 12px; color: var(--text-dim); font-size: var(--type-sm); }
.legend { display: flex; flex-wrap: wrap; align-items: center; gap: 2px 12px; }.legend span { display: flex; align-items: center; gap: 6px; }.legend i { width: 17px; height: 2px; background: var(--cut-sample); }.legend .rapid { height: 0; border-top: 1px dashed var(--rapid-sample); background: none; }
.legend label { display: flex; align-items: center; gap: 4px; min-height: var(--touch); cursor: pointer; }
.views { display: flex; align-items: center; flex-wrap: wrap; gap: 2px; margin-left: auto; }
.views .choice { display: inline-flex; justify-content: center; align-items: center; min-width: var(--touch); min-height: var(--touch); padding: 0 9px; border: 0; border-radius: 7px; background: none; color: var(--text-dim); font-size: var(--type-body); cursor: pointer; }
.views .choice:hover { background: var(--control-hover); }
.views .choice:active { background: var(--control-active); }
.travel-control { padding-inline: 9px; }
.views :deep(svg) { display: block; width: 16px; height: 16px; }
.banners, .preview-banner { position: absolute; left: 18px; max-width: calc(100% - 36px); pointer-events: none; font-size: var(--type-sm); color: var(--text-dim); }
.banners { top: 12px; display: flex; flex-direction: column; align-items: flex-start; gap: 6px; }
.preview-banner { bottom: 12px; }
.banners span, .preview-banner { padding: 5px 8px; border-radius: 6px; background: var(--panel); }
.stop { color: var(--stop); }.warn { color: var(--warn); }
</style>
