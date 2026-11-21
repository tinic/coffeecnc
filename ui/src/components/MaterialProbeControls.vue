<script setup lang="ts">
import { computed, onBeforeUnmount, shallowRef, watch } from 'vue'
import { allHomed, homingActive, manualBlock, onResult, panel, send } from '../machine'
import { homingMessage } from '../machine-block'
import type { CommandBody } from '../protocol'
import { setterState, touchedOff, type SetterStatusLine } from '../setter-state'
import { formatLength, wcsName } from '../units'
import SetterStatusBand from './SetterStatusBand.vue'
import WorkpieceProbeDiagram from './WorkpieceProbeDiagram.vue'
import ProbeGuideDialog from './ProbeGuideDialog.vue'
import { angleChoices, type AngleEdge, type MaterialGuideTarget } from '../probe-guide'
import ModalDialog from './ModalDialog.vue'
import { vTooltip } from '../tooltip'

const props = defineProps<{ tool: number }>()
type Request = Extract<CommandBody, { type: 'fixed_setter'; action: 'material' | 'corner' | 'ball' | 'bore' | 'boss' | 'angle' | 'apply_angle' | 'inside_corner' | 'pocket' | 'slot' | 'ridge' }>
// The button names the part face. The protocol still specifies the tool's travel.
const surfaces = [
  { label: 'Z top', travel: 'z_top', surface: 'top surface', approach: '−Z' },
  { label: 'X−', travel: 'x_plus', surface: 'left edge', approach: '+X' },
  { label: 'X+', travel: 'x_minus', surface: 'right edge', approach: '−X' },
  { label: 'Y−', travel: 'y_plus', surface: 'front edge', approach: '+Y' },
  { label: 'Y+', travel: 'y_minus', surface: 'back edge', approach: '−Y' },
] as const
// A corner probes the two faces that meet there; its name follows the edge buttons.
const corners = [
  { label: 'X− Y−', name: 'front-left corner', x: 'x_plus', y: 'y_plus' },
  { label: 'X+ Y−', name: 'front-right corner', x: 'x_minus', y: 'y_plus' },
  { label: 'X− Y+', name: 'back-left corner', x: 'x_plus', y: 'y_minus' },
  { label: 'X+ Y+', name: 'back-right corner', x: 'x_minus', y: 'y_minus' },
] as const
const insideCorners = [
  { label: 'X− Y−', x: 'x_minus', y: 'y_minus' },
  { label: 'X+ Y−', x: 'x_plus', y: 'y_minus' },
  { label: 'X− Y+', x: 'x_minus', y: 'y_plus' },
  { label: 'X+ Y+', x: 'x_plus', y: 'y_plus' },
] as const
const featureAxes = ['x', 'y'] as const
const narrowFeatures = ['slot', 'ridge'] as const
const pending = shallowRef<Request | null>(null)
const pendingCorner = computed(() => pending.value?.action === 'corner' ? [pending.value.x_direction, pending.value.y_direction] : null)
const hint: SetterStatusLine = {
  text: '',
  level: 'info',
}
const line = shallowRef(hint)
const toolBlock = computed(() => panel.link === 'live' && panel.status !== null &&
  (panel.status.tool.in_spindle !== props.tool || panel.status.wait.kind === 'tool_change')
  ? `Load T${String(props.tool)} to probe material.` : null)
const displayLine = computed((): SetterStatusLine => line.value === hint && toolBlock.value !== null
  ? { text: toolBlock.value, level: 'info' } : line.value)
const machineReason = computed(() => manualBlock.value ?? (homingActive.value ? homingMessage : null))
const spindleStopped = computed(() => {
  const spindle = panel.status?.spindle
  return spindle?.direction === 'stopped' && typeof spindle.actual_rpm === 'number' &&
    Math.abs(spindle.actual_rpm) < 1
})
watch([
  machineReason, toolBlock, spindleStopped, (): boolean | undefined => panel.status?.motion.in_position,
  (): boolean | null | undefined => panel.status?.probe_input, (): boolean => touchedOff(props.tool),
], () => {
  if (line.value.level === 'blocked') line.value = hint
})
let statusRequest: CommandBody | null = null

const guide = shallowRef<MaterialGuideTarget | null>(null)
const choosingFeature = shallowRef(false)
function openGuide(target: MaterialGuideTarget): void {
  if (pending.value === null) {
    choosingFeature.value = false
    guide.value = target
  }
}
function startGuide(size: number, angle: AngleEdge): void {
  const target = guide.value
  guide.value = null
  if (target !== null) probe(target, size, angle)
}
const askingRotation = shallowRef(false)
const edgeRotation = shallowRef<{ id: number; wcs: number; degrees: number; frame: string } | null>(null)
const frame = computed(() => JSON.stringify(panel.status?.offsets))
const shifted = computed(() => panel.status?.offsets.g92.some((value) => value !== 0) === true)
const canApplyRotation = computed(() => edgeRotation.value !== null && frame.value === edgeRotation.value.frame &&
  panel.status?.offsets.wcs === edgeRotation.value.wcs && !pending.value && !machineReason.value && !toolBlock.value)
watch(frame, () => {
  edgeRotation.value = null
  askingRotation.value = false
})
const degreesText = (degrees: number): string => `${degrees >= 0 ? '+' : '−'}${Math.abs(degrees).toFixed(3)}°`
function applyRotation(): void {
  const offer = edgeRotation.value
  if (!canApplyRotation.value || !offer) return
  askingRotation.value = false
  const request = { type: 'fixed_setter', action: 'apply_angle', measurement_id: offer.id, tool: props.tool } as const
  pending.value = request
  line.value = { text: `Setting ${wcsName(offer.wcs)} rotation…`, level: 'busy' }
  send(request)
}

function probe(target: MaterialGuideTarget, size: number, angle: AngleEdge): void {
  if (pending.value) return
  edgeRotation.value = null
  askingRotation.value = false
  const status = panel.status
  const [x, y, z] = ['x', 'y', 'z'].map((axis) => status?.axes.find((entry) => entry.axis === axis)?.machine)
  let reason = machineReason.value
  if (reason === null && (status?.tool.in_spindle !== props.tool || status.wait.kind === 'tool_change'))
    reason = toolBlock.value ?? `Load T${String(props.tool)} to probe material.`
  if (reason === null && !spindleStopped.value)
    reason = 'Wait for the spindle to stop.'
  if (reason === null && !status?.motion.in_position) reason = 'Wait for the axes to stop.'
  if (reason === null && !touchedOff(props.tool)) {
    const name = `T${String(props.tool)}`
    if (target.kind === 'surface' && target.direction === 'z_top')
      reason = `Touch off ${name} at the tool setter before probing Z.`
    if (target.kind === 'corner')
      reason = `Touch off ${name} and probe Z top before probing a corner.`
    if (target.kind === 'boss' || target.kind === 'ridge' || target.kind === 'ball')
      reason = `Touch off ${name} and probe Z top on the ${target.kind === 'ball' ? 'gauge' : target.kind} first.`
  }
  if (reason !== null || x === undefined || y === undefined || z === undefined || ![x, y, z].every(Number.isFinite)) {
    line.value = { text: reason ?? 'Waiting for the machine position.', level: 'blocked' }
    return
  }
  const position = { type: 'fixed_setter', tool: props.tool, x, y, z } as const
  let request: Request
  let activity: string
  switch (target.kind) {
    case 'surface':
      request = { ...position, action: 'material', direction: target.direction }
      activity = surfaces.find(entry => entry.travel === target.direction)?.surface ?? 'surface'
      break
    case 'corner':
    case 'inside_corner':
      request = { ...position, action: target.kind, x_direction: target.x, y_direction: target.y }
      activity = target.kind === 'inside_corner' ? 'the inside corner'
        : `the ${corners.find(entry => entry.x === target.x && entry.y === target.y)?.name ?? 'corner'}`
      break
    case 'angle':
      request = { ...position, action: 'angle', direction: angle, spacing: size }
      activity = `the ${angleChoices.find(entry => entry.travel === angle)?.name.toLowerCase() ?? 'selected'} edge at two points`
      break
    case 'ball':
      request = { ...position, action: 'ball', gauge_width: size }
      activity = 'both sides of the gauge'
      break
    case 'boss':
      request = { ...position, action: 'boss', diameter: size }
      activity = 'the boss sides'
      break
    case 'bore':
    case 'pocket':
      request = { ...position, action: target.kind }
      activity = `the ${target.kind} walls`
      break
    case 'slot':
      request = { ...position, action: 'slot', axis: target.axis }
      activity = `the slot across ${target.axis.toUpperCase()}`
      break
    case 'ridge':
      request = { ...position, action: 'ridge', axis: target.axis, width: size }
      activity = `the ridge across ${target.axis.toUpperCase()}`
      break
  }
  pending.value = request
  line.value = { text: `Probing ${activity}…`, level: 'busy' }
  send(request)
}

const removeListener = onResult((command, result) => {
  if (command === statusRequest) {
    statusRequest = null
    if (result.outcome === 'accepted') setterState.value = result.setter ?? null
    return
  }
  if (command !== pending.value) return
  const request = pending.value
  pending.value = null
  if (result.outcome === 'timed_out') send({ type: 'abort' })
  if (request.action === 'apply_angle') {
    const degrees = result.setter?.edge_angle
    edgeRotation.value = null
    if (result.outcome === 'accepted' && degrees !== undefined) {
      setterState.value = result.setter ?? null
      line.value = { text: `Work rotation set to ${degreesText(degrees)}.`, level: 'done' }
    } else {
      line.value = { text: result.reason || 'The machine did not confirm the rotation.', level: 'blocked' }
    }
    return
  }
  if (request.action === 'angle') {
    const degrees = result.setter?.edge_angle
    if (result.outcome !== 'accepted' || degrees === undefined) {
      line.value = { text: result.reason || 'The machine did not report the edge angle.', level: 'blocked' }
      return
    }
    if (result.setter) setterState.value = result.setter
    const rise = Math.tan(degrees * Math.PI / 180) * request.spacing
    const face = angleChoices.find((entry) => entry.travel === request.direction)?.name ?? 'Measured'
    const offer = result.setter?.edge_rotation
    if (offer) edgeRotation.value = { id: offer.measurement_id, wcs: offer.wcs, degrees, frame: frame.value }
    line.value = { text: `${face} edge turns ${degreesText(degrees)} (${Math.abs(rise).toFixed(3)} mm over ${String(Math.abs(request.spacing))} mm).`, level: 'done' }
    return
  }
  if (request.action === 'bore' || request.action === 'boss') {
    const measured = result.setter?.center_diameter
    if (result.outcome !== 'accepted' || measured === undefined || !result.setter?.material) {
      line.value = { text: result.reason || 'The machine did not confirm the work origin.', level: 'blocked' }
      return
    }
    setterState.value = result.setter
    line.value = { text: `${wcsName(result.setter.material.wcs)} X0 Y0 set at the ${request.action} centre, Ø ${measured.toFixed(3)} mm.`, level: 'done' }
    return
  }
  if (request.action === 'ball') {
    const measured = result.setter?.ball_diameter
    if (result.outcome !== 'accepted' || measured === undefined) {
      line.value = { text: result.reason || 'The machine did not confirm the ball diameter.', level: 'blocked' }
      return
    }
    if (result.setter) setterState.value = result.setter
      line.value = { text: `T${String(props.tool)} ball Ø${measured.toFixed(3)} mm saved to the tool table.`, level: 'done' }
    return
  }
  const material = result.setter?.material
  if (result.outcome !== 'accepted' || !material) {
    line.value = { text: result.reason || 'The machine did not confirm the work origin.', level: 'blocked' }
    return
  }
  if (result.setter) setterState.value = result.setter
  if (request.action === 'corner') {
    const corner = corners.find((entry) => entry.x === request.x_direction && entry.y === request.y_direction)
    line.value = { text: `${wcsName(material.wcs)} X0 Y0 set at the ${corner?.label ?? ''} ${corner?.name ?? 'corner'}.`, level: 'done' }
    return
  }
  if (request.action === 'inside_corner') {
    const corner = insideCorners.find(entry => entry.x === request.x_direction && entry.y === request.y_direction)
    line.value = { text: `${wcsName(material.wcs)} X0 Y0 set at the ${corner?.label ?? ''} inside corner.`, level: 'done' }
    return
  }
  if (request.action === 'pocket' || request.action === 'slot' || request.action === 'ridge') {
    const size = result.setter?.feature_size
    const format = (value: number): string => `${formatLength(value, panel.unit)} ${panel.unit === 'inch' ? 'in' : 'mm'}`
    const width = request.action === 'pocket' ? undefined : size?.[request.axis]
    const span = request.action === 'pocket' && size?.x !== undefined && size.y !== undefined
      ? `X ${format(size.x)} × Y ${format(size.y)}` : width !== undefined ? format(width) : null
    if (!span) {
      line.value = { text: 'The machine did not report the measured width.', level: 'blocked' }
      return
    }
    const axes = material.axis === 'XY' ? 'X0 Y0' : `${material.axis}0`
    line.value = { text: `${wcsName(material.wcs)} ${axes} set at the ${request.action} centre · ${span}`, level: 'done' }
    return
  }
  const surface = surfaces.find((entry) => entry.travel === request.direction)
  const face = request.direction === 'z_top' ? 'the top surface' : `the ${surface?.label ?? ''} ${surface?.surface ?? 'edge'}`
  line.value = { text: `${wcsName(material.wcs)} ${material.axis}0 set at ${face}.`, level: 'done' }
})
watch([
  (): unknown => panel.hello, (): string => panel.link, (): string | undefined => panel.status?.machine,
  allHomed, homingActive,
], () => {
  edgeRotation.value = null
  askingRotation.value = false
  if (!panel.hello?.fixed_setter) return
  setterState.value = null
  statusRequest = null
  if (panel.link !== 'live') return
  statusRequest = { type: 'fixed_setter', action: 'status' }
  send(statusRequest)
}, { immediate: true })
onBeforeUnmount(() => {
  if (pending.value) send({ type: 'abort' })
  removeListener()
})
</script>

<template>
  <section class="material-probe" aria-label="Material probe">
    <div class="probe-layout">
      <div class="probe-actions">
        <button
          v-for="face in surfaces" :key="face.travel"
          v-tooltip="`Probe the ${face.surface}. Travel ${face.approach}.`"
          type="button" class="btn" :class="{ on: pending?.action === 'material' && pending.direction === face.travel }"
          @click="openGuide({ kind: 'surface', direction: face.travel })"
        >
          {{ face.label }}
        </button>
        <button
          v-for="corner in corners" :key="corner.label"
          v-tooltip="`Probe the ${corner.name}: X then Y face, from over the part.`"
          type="button" class="btn corner" :class="{ on: pending?.action === 'corner' && pending.x_direction === corner.x && pending.y_direction === corner.y }"
          @click="openGuide({ kind: 'corner', x: corner.x, y: corner.y })"
        >
          {{ corner.label }}
        </button>
      </div>
      <WorkpieceProbeDiagram :travel="pending?.action === 'material' ? pending.direction : null" :corner="pendingCorner" />
      <div class="centres">
        <button
          v-tooltip="'Jog the ball into the bore, below its top, then press. Sets X0 Y0 at the centre.'"
          type="button" class="btn" :class="{ on: pending?.action === 'bore' }" @click="openGuide({ kind: 'bore' })"
        >
          Bore centre
        </button>
        <button
          v-tooltip="'Probe Z top on the boss, jog the ball over its middle, then enter its approximate diameter.'"
          type="button" class="btn" :class="{ on: pending?.action === 'boss' }" @click="openGuide({ kind: 'boss' })"
        >
          Boss centre…
        </button>
        <button
          v-tooltip="'Measure how far the front or left edge turns.'"
          type="button" class="btn" :class="{ on: pending?.action === 'angle' }" @click="openGuide({ kind: 'angle' })"
        >
          Edge angle…
        </button>
      </div>
      <div class="probe-utilities">
        <button
          v-tooltip="'Measure the ball on a gauge of known width: probe Z top on the gauge, jog the ball over its middle, then enter the width.'"
          type="button" class="btn" :class="{ on: pending?.action === 'ball' }" @click="openGuide({ kind: 'ball' })"
        >
          Calibrate ball…
        </button>
        <button
          v-if="panel.hello?.fixed_setter?.material_features" class="btn" type="button" aria-haspopup="dialog"
          :class="{ on: pending?.action === 'inside_corner' || pending?.action === 'pocket' || pending?.action === 'slot' || pending?.action === 'ridge' }"
          @click="pending === null && (choosingFeature = true)"
        >
          Features…
        </button>
      </div>
    </div>
    <SetterStatusBand :line="displayLine">
      <template #actions>
        <button v-if="canApplyRotation" class="btn rotation-action" type="button" @click="askingRotation = true">Apply rotation…</button>
      </template>
    </SetterStatusBand>
    <ProbeGuideDialog
      v-if="guide" :target="guide" :limits="panel.hello?.fixed_setter" :tool="tool"
      @start="startGuide" @close="guide = null"
    />
    <ModalDialog v-if="choosingFeature" title="Probe a feature" size="medium" fresh-gesture @close="choosingFeature = false">
      <div class="feature-options">
        <div class="feature-row">
          <span>Inside corner</span>
          <div class="feature-buttons">
            <button
              v-for="corner in insideCorners" :key="corner.label" class="btn" type="button"
              :aria-label="'Inside corner ' + corner.label" @click="openGuide({ kind: 'inside_corner', x: corner.x, y: corner.y })"
            >
              {{ corner.label }}
            </button>
          </div>
        </div>
        <div class="feature-row">
          <span>Rectangular pocket</span>
          <button class="btn" type="button" @click="openGuide({ kind: 'pocket' })">Centre and size</button>
        </div>
        <div v-for="kind in narrowFeatures" :key="kind" class="feature-row">
          <span class="feature-name">{{ kind }}</span>
          <div class="feature-buttons">
            <button
              v-for="axis in featureAxes" :key="axis" class="btn" type="button" :aria-label="kind + ' across ' + axis.toUpperCase()"
              @click="openGuide({ kind, axis })"
            >
              Across {{ axis.toUpperCase() }}
            </button>
          </div>
        </div>
      </div>
    </ModalDialog>
    <ModalDialog v-if="askingRotation && edgeRotation" title="Apply as G5x rotation?" size="small" fresh-gesture @close="askingRotation = false">
      <p class="rotation-effect">Rotate the {{ wcsName(edgeRotation.wcs) }} XY axes {{ degreesText(edgeRotation.degrees) }} about its stored origin.</p>
      <p v-if="shifted" class="rotation-effect">The current G52/G92 shift stays active.</p>
      <template #footer>
        <button class="btn" type="button" @click="askingRotation = false">Cancel</button>
        <button class="btn primary" type="button" :disabled="!canApplyRotation" @click="applyRotation">Apply rotation</button>
      </template>
    </ModalDialog>
  </section>
</template>

<style scoped>
.rotation-action { flex: none; min-height: 32px; padding-inline: 8px; font-size: 12px; }
.rotation-effect { margin: 0; line-height: 1.5; }
.material-probe { min-width: 0; container-type: inline-size; }
.probe-layout { display: grid; grid-template-columns: minmax(0, 1fr) minmax(186px, 0.82fr); align-items: center; gap: 12px; }
.probe-actions { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 8px; }
.probe-actions button:first-child { grid-column: 1 / -1; }
.probe-actions button { min-height: 56px; min-width: 0; padding-inline: 4px; white-space: nowrap; }
.probe-actions button.corner { min-height: 44px; }
.probe-utilities { display: flex; gap: 8px; grid-column: 2; align-self: start; }
.probe-utilities button { flex: 1; min-height: 44px; padding-inline: 5px; }
.feature-options { display: grid; gap: 12px; }
.feature-row { display: grid; grid-template-columns: minmax(108px, .6fr) 1.4fr; gap: 12px; align-items: center; }
.feature-name { text-transform: capitalize; }
.feature-buttons { display: flex; flex-wrap: wrap; gap: 6px; }
.feature-buttons button { flex: 1; white-space: nowrap; }
.feature-row button { min-height: 44px; }
.centres { display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; }
.centres button { min-height: 44px; }
@container (max-width: 390px) {
  .probe-layout { grid-template-columns: minmax(0, 1fr); gap: 8px; }
  .probe-actions { grid-template-columns: repeat(5, minmax(0, 1fr)); }
  .probe-actions button.corner { grid-column: span 1; }
  .probe-actions button:first-child { grid-column: auto; }
  .probe-utilities { grid-column: 1; }
}
</style>
