<script setup lang="ts">
// The path view: the planned path from the preview, the part of it the machine has executed, the
// tool position the machine reports, the active work origin and the machine travel.
//
// It is a picture only. Dragging, zooming and the view buttons change the camera and nothing
// else; no gesture on it sends a command.
import { computed, onBeforeUnmount, onMounted, ref, shallowRef, useTemplateRef, watch } from 'vue'

import { findTool, live, panel } from '../machine'
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
import type { Preview, Vec3 } from '../protocol'
import AppIcon from './AppIcon.vue'

const canvas = useTemplateRef<HTMLCanvasElement>('canvas')
const size = ref<{ width: number; height: number }>({ width: 0, height: 0 })
const camera = shallowRef<Camera>({ ...views.iso, scale: 1, centre: [200, 125, -100] })
const view = ref<ViewName | null>('iso')
/** Refit when the path changes, until the operator moves the camera. */
const autoFit = ref<boolean>(true)

/** The last preview that had a path, kept so an outdated path can be shown as outdated. */
const drawn = shallowRef<Preview | null>(null)

watch(
  () => panel.preview,
  (preview) => {
    if (preview === null || preview.state === 'unavailable') {
      drawn.value = null
    } else if (preview.state !== 'pending') {
      drawn.value = preview
    }
  },
  { immediate: true },
)

watch(
  () => panel.program?.revision,
  () => {
    // Another program: its predecessor's path says nothing about it.
    if (drawn.value !== null && drawn.value.context !== panel.preview?.context) {
      drawn.value = null
    }
    autoFit.value = true
  },
)

const current = computed(
  () => drawn.value !== null && panel.status !== null && drawn.value.context === panel.status.preview_context,
)

const banner = computed((): { text: string; tone: '' | 'warn' | 'stop' } => {
  const preview = panel.preview
  if (preview === null || preview.state === 'unavailable') {
    return { text: (panel.status?.program.file ?? null) === null ? 'No program loaded' : 'No preview available', tone: '' }
  }
  if (preview.state === 'pending') {
    return { text: 'Computing preview', tone: 'warn' }
  }
  if (!current.value) {
    return { text: 'Preview out of date', tone: 'warn' }
  }
  if (preview.state === 'failed') {
    return {
      text: `Preview failed at line ${String(preview.error?.line ?? 0)}: path shown up to there`,
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

const toolTip = computed((): Vec3 | null => {
  const [x, y, z] = panel.status?.axes ?? []
  if (x === undefined || y === undefined || z === undefined || !(x.homed && y.homed && z.homed)) {
    // Before homing the machine position is not known, so there is nothing to place.
    return null
  }
  const t = toolOffset.value
  return [x.machine - t[0], y.machine - t[1], z.machine - t[2]]
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

function toolName(number: number): string {
  if (number === 0) {
    return 'No tool'
  }
  const comment = findTool(number)?.comment ?? ''
  return comment === '' ? `T${String(number)}` : `T${String(number)} ${comment}`
}

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
  if (points.length === 0 && travelBox.value !== null) {
    points = boxCorners(travelBox.value.min, travelBox.value.max)
  }
  camera.value = fit(camera.value, points, width, height)
}

function fitButton(what: 'path' | 'machine'): void {
  autoFit.value = what === 'path'
  fitTo(what)
}

function setView(name: ViewName): void {
  view.value = name
  camera.value = { ...camera.value, ...views[name] }
  fitTo('path')
}

watch([drawn, size], () => {
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
  travel: '#3b434d',
  rapid: '#7b8794',
  executed: '#f2f4f6',
  marker: '#f0b64a',
  outdated: '#66707c',
  axes: ['#d97b7b', '#7fbf8a', '#7b9fe0'],
} as const

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

  const box = travelBox.value
  if (box !== null) {
    const corners = boxCorners(box.min, box.max).map(at)
    context.strokeStyle = colours.travel
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
        const colour = fresh ? toolColour(tools.value.indexOf(segment.tool)) : colours.outdated
        if (kind === 'rapid') {
          context.setLineDash([3, 5])
          context.strokeStyle = fresh ? colours.rapid : colours.outdated
          context.globalAlpha = done ? 0.9 : 0.5
          context.lineWidth = 1
        } else if (kind === 'done') {
          context.setLineDash([])
          context.strokeStyle = segment.line === motionLine ? colours.executed : colour
          context.globalAlpha = 1
          context.lineWidth = 2.6
        } else {
          context.setLineDash([])
          context.strokeStyle = colour
          context.globalAlpha = fresh ? (running ? 0.45 : 0.9) : 0.5
          context.lineWidth = 1.3
        }
        context.stroke()
      }
    }
    context.globalAlpha = 1
    context.setLineDash([])
  }

  if (status !== null) {
    // The active work origin: where the work coordinates are zero.
    const { g5x, g92, rotation_xy: degrees } = status.offsets
    const a = (degrees * Math.PI) / 180
    const origin: Vec3 = [
      g5x[0] + g92[0] * Math.cos(a) - g92[1] * Math.sin(a),
      g5x[1] + g92[0] * Math.sin(a) + g92[1] * Math.cos(a),
      g5x[2] + g92[2],
    ]
    const directions: Vec3[] = [
      [Math.cos(a), Math.sin(a), 0],
      [-Math.sin(a), Math.cos(a), 0],
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
      const colour = colours.axes[index] ?? colours.rapid
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
    const colour = live.value ? colours.marker : colours.outdated
    context.strokeStyle = colour
    context.fillStyle = colour
    if (length > 0.01) {
      // The tool's axis: a fixed length on screen, pointing up the machine's Z.
      context.lineWidth = 3
      context.beginPath()
      context.moveTo(x, y)
      context.lineTo(x + ((top[0] - x) / length) * 30 * (length / cam.scale), y + ((top[1] - y) / length) * 30 * (length / cam.scale))
      context.stroke()
    }
    context.lineWidth = 2
    context.beginPath()
    context.arc(x, y, 6, 0, 2 * Math.PI)
    context.stroke()
    context.beginPath()
    context.arc(x, y, 2, 0, 2 * Math.PI)
    context.fill()
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

watch([camera, size, drawn, current, (): unknown => panel.status, (): unknown => panel.link, tools], schedule)

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
    <canvas
      ref="canvas"
      @pointerdown="pointerDown"
      @pointermove="pointerMove"
      @pointerup="pointerUp"
      @pointercancel="pointerUp"
      @wheel.prevent="wheel"
      @contextmenu.prevent
    ></canvas>

    <div class="top">
      <span class="chip" title="The path comes from the interpreter's dry run of the program. It is not a measurement.">
        {{ panel.simulated ? 'Simulated preview' : 'Preview' }}
      </span>
      <span v-if="banner.text" class="chip" :class="banner.tone">{{ banner.text }}</span>
      <span v-if="!live" class="chip stop">Tool position not live</span>
      <span v-else-if="toolTip === null" class="chip warn">Tool position unknown: not homed</span>
    </div>

    <div class="views">
      <div class="segmented">
        <button class="btn" type="button" :class="{ on: view === 'iso' }" @click="setView('iso')">Iso</button>
        <button class="btn" type="button" :class="{ on: view === 'top' }" @click="setView('top')">Top</button>
        <button class="btn" type="button" :class="{ on: view === 'front' }" @click="setView('front')">Front</button>
      </div>
      <button class="btn" type="button" title="Fit the path in the view" @click="fitButton('path')">
        <AppIcon name="fit" />Path
      </button>
      <button class="btn" type="button" title="Fit the machine travel in the view" @click="fitButton('machine')">
        <AppIcon name="fit" />Machine
      </button>
    </div>

    <div class="legend">
      <span v-for="(tool, index) in tools" :key="tool" class="key">
        <span class="swatch" :style="{ background: current ? toolColour(index) : colours.outdated }"></span>
        {{ toolName(tool) }}
      </span>
      <span class="key"><span class="swatch dashed"></span>Rapid</span>
      <span class="key"><span class="swatch box"></span>Travel of the tool tip</span>
      <span class="key"><span class="swatch marker"></span>Tool, as reported</span>
    </div>
  </div>
</template>

<style scoped>
.pathview {
  position: relative;
  min-width: 0;
  min-height: 0;
  border: 1px solid var(--line);
  border-radius: var(--radius);
  background: var(--bg-sunken);
  overflow: hidden;
}

.pathview.outdated {
  border-style: dashed;
  border-color: var(--warn);
}

canvas {
  display: block;
  width: 100%;
  height: 100%;
  cursor: grab;
  touch-action: none;
}

.top {
  position: absolute;
  top: 8px;
  left: 8px;
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  flex-direction: column;
  align-items: flex-start;
  pointer-events: none;
}

.top .chip {
  background: var(--panel);
  pointer-events: auto;
}

.top .chip.warn {
  background: #2b2619;
}

.top .chip.stop {
  background: #2e1c1d;
}

.views {
  position: absolute;
  right: 8px;
  bottom: 8px;
  display: flex;
  gap: 6px;
}

.views .btn {
  padding: 0 10px;
  background: var(--panel);
  font-size: 13px;
}

.views .btn.on {
  background: #1f2c44;
}

.legend {
  position: absolute;
  top: 8px;
  right: 8px;
  display: flex;
  flex-direction: column;
  gap: 3px;
  padding: 6px 10px;
  border: 1px solid var(--line);
  border-radius: var(--radius);
  background: rgb(27 31 36 / 85%);
  font-size: 12px;
  color: var(--text-dim);
  pointer-events: none;
}

.key {
  display: flex;
  align-items: center;
  gap: 6px;
}

.swatch {
  width: 18px;
  height: 3px;
  border-radius: 2px;
}

.swatch.dashed {
  height: 0;
  border-top: 2px dashed #7b8794;
  border-radius: 0;
}

.swatch.box {
  height: 9px;
  border: 1px solid #56606c;
  border-radius: 0;
}

.swatch.marker {
  width: 10px;
  height: 10px;
  margin: 0 4px;
  border: 2px solid #f0b64a;
  border-radius: 50%;
}
</style>
