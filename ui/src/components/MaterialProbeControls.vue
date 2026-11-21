<script setup lang="ts">
import { computed, onBeforeUnmount, shallowRef, watch } from 'vue'
import { allHomed, findTool, homingActive, manualBlock, onResult, panel, send } from '../machine'
import { homingMessage } from '../machine-block'
import type { CommandBody, ProbeDirection } from '../protocol'
import { setterState, touchedOff, type SetterStatusLine } from '../setter-state'
import { wcsName } from '../units'
import SetterStatusBand from './SetterStatusBand.vue'
import WorkpieceProbeDiagram from './WorkpieceProbeDiagram.vue'
import NumberDialog from './NumberDialog.vue'
import ModalDialog from './ModalDialog.vue'
import { vTooltip } from '../tooltip'

const props = defineProps<{ tool: number }>()
type Request = Extract<CommandBody, { type: 'fixed_setter'; action: 'material' | 'corner' | 'ball' | 'bore' | 'boss' | 'angle' | 'apply_angle' }>
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
type Corner = (typeof corners)[number]
const pending = shallowRef<Request | null>(null)
const pendingCorner = computed(() => pending.value?.action === 'corner' ? [pending.value.x_direction, pending.value.y_direction] : null)
const hint: SetterStatusLine = {
  text: 'Edges: jog beside the part. Corners: probe Z top, then jog over the part within 10 mm of both edges.',
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

// Ball calibration: the operator enters the gauge width; the probe starts over the gauge.
const askingGauge = shallowRef(false)
function calibrate(width: number): void {
  askingGauge.value = false
  probe('x_plus', undefined, { kind: 'ball', size: width })
}
// Bore and boss centres; a boss needs its approximate diameter to go round it.
const askingBoss = shallowRef(false)
function centreBoss(diameter: number): void {
  askingBoss.value = false
  probe('x_plus', undefined, { kind: 'boss', size: diameter })
}
interface Feature { kind: 'ball' | 'boss' | 'bore' | 'angle'; size: number }
const angleFaces = [
  { label: 'Front · Y−', name: 'Front', travel: 'y_plus', along: 'X' },
  { label: 'Left · X−', name: 'Left', travel: 'x_plus', along: 'Y' },
] as const
const angleFace = shallowRef<(typeof angleFaces)[number]>(angleFaces[0])
const askingAngle = shallowRef(false)
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
function measureAngle(spacing: number): void {
  askingAngle.value = false
  probe(angleFace.value.travel, undefined, { kind: 'angle', size: spacing })
}
function applyRotation(): void {
  const offer = edgeRotation.value
  if (!canApplyRotation.value || !offer) return
  askingRotation.value = false
  const request = { type: 'fixed_setter', action: 'apply_angle', measurement_id: offer.id, tool: props.tool } as const
  pending.value = request
  line.value = { text: `Setting ${wcsName(offer.wcs)} rotation…`, level: 'busy' }
  send(request)
}

function probe(direction: ProbeDirection, corner?: Corner, feature?: Feature): void {
  const gauge = feature?.kind === 'ball' ? feature.size : undefined
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
  if (reason === null && direction === 'z_top' && !touchedOff(props.tool))
    reason = `Touch off T${String(props.tool)} at the tool setter before probing Z.`
  if (reason === null && corner !== undefined && !touchedOff(props.tool))
    reason = `Touch off T${String(props.tool)} and probe Z top before probing a corner.`
  if (reason === null && gauge !== undefined && !touchedOff(props.tool))
    reason = `Touch off T${String(props.tool)} and probe Z top on the gauge first.`
  if (reason === null && feature?.kind === 'boss' && !touchedOff(props.tool))
    reason = `Touch off T${String(props.tool)} and probe Z top on the boss first.`
  if (reason !== null || x === undefined || y === undefined || z === undefined || ![x, y, z].every(Number.isFinite)) {
    line.value = { text: reason ?? 'Waiting for the machine position.', level: 'blocked' }
    return
  }
  const request: Request = feature?.kind === 'angle'
    ? { type: 'fixed_setter', action: 'angle', direction: angleFace.value.travel, spacing: feature.size, tool: props.tool, x, y, z }
    : feature?.kind === 'bore'
    ? { type: 'fixed_setter', action: 'bore', tool: props.tool, x, y, z }
    : feature?.kind === 'boss'
    ? { type: 'fixed_setter', action: 'boss', diameter: feature.size, tool: props.tool, x, y, z }
    : gauge !== undefined
    ? { type: 'fixed_setter', action: 'ball', gauge_width: gauge, tool: props.tool, x, y, z }
    : corner === undefined
      ? { type: 'fixed_setter', action: 'material', direction, tool: props.tool, x, y, z }
      : { type: 'fixed_setter', action: 'corner', x_direction: corner.x, y_direction: corner.y, tool: props.tool, x, y, z }
  pending.value = request
  const surface = surfaces.find((entry) => entry.travel === direction)
  line.value = { text: feature?.kind === 'angle' ? `Probing the ${angleFace.value.name.toLowerCase()} edge at two points…`
    : feature?.kind === 'bore' ? 'Probing the bore walls…'
    : feature?.kind === 'boss' ? 'Probing the boss sides…'
    : gauge !== undefined ? 'Probing both sides of the gauge…'
    : `Probing ${corner === undefined ? surface?.surface ?? 'surface' : `the ${corner.name}`}…`, level: 'busy' }
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
    const face = angleFaces.find((entry) => entry.travel === request.direction)?.name ?? 'Measured'
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
          @click="probe(face.travel)"
        >
          {{ face.label }}
        </button>
        <button
          v-for="corner in corners" :key="corner.label"
          v-tooltip="`Probe the ${corner.name}: X then Y face, from over the part.`"
          type="button" class="btn corner" :class="{ on: pending?.action === 'corner' && pending.x_direction === corner.x && pending.y_direction === corner.y }"
          @click="probe(corner.x, corner)"
        >
          {{ corner.label }}
        </button>
      </div>
      <WorkpieceProbeDiagram :travel="pending?.action === 'material' ? pending.direction : null" :corner="pendingCorner" />
      <div class="centres">
        <button
          v-tooltip="'Jog the ball into the bore, below its top, then press. Sets X0 Y0 at the centre.'"
          type="button" class="btn" :class="{ on: pending?.action === 'bore' }" @click="probe('x_plus', undefined, { kind: 'bore', size: 0 })"
        >
          Bore centre
        </button>
        <button
          v-tooltip="'Probe Z top on the boss, jog the ball over its middle, then enter its approximate diameter.'"
          type="button" class="btn" :class="{ on: pending?.action === 'boss' }" @click="askingBoss = true"
        >
          Boss centre…
        </button>
        <button
          v-tooltip="'Measure how far the front or left edge turns.'"
          type="button" class="btn" :class="{ on: pending?.action === 'angle' }" @click="askingAngle = true"
        >
          Edge angle…
        </button>
      </div>
      <button
        v-tooltip="'Measure the ball on a gauge of known width: probe Z top on the gauge, jog the ball over its middle, then enter the width.'"
        type="button" class="btn calibrate" :class="{ on: pending?.action === 'ball' }" @click="askingGauge = true"
      >
        Calibrate ball…
      </button>
    </div>
    <SetterStatusBand :line="displayLine">
      <template #actions>
        <button v-if="canApplyRotation" class="btn rotation-action" type="button" @click="askingRotation = true">Apply rotation…</button>
      </template>
    </SetterStatusBand>
    <NumberDialog
      v-if="askingAngle"
      title="Measure edge angle"
      :effect="`Jog beside the ${angleFace.name.toLowerCase()} edge. Enter the distance to the second touch along ${angleFace.along}.`"
      :label="`Distance along ${angleFace.along}`" unit="mm" initial="50" action="Probe" @confirm="measureAngle" @close="askingAngle = false"
    >
      <template #options>
        <div class="edge-choice" role="group" aria-label="Edge to measure">
          <button
            v-for="face in angleFaces" :key="face.travel" class="btn" type="button"
            :class="{ on: angleFace.travel === face.travel }" :aria-pressed="angleFace.travel === face.travel" @click="angleFace = face"
          >
            {{ face.label }}
          </button>
        </div>
      </template>
    </NumberDialog>
    <ModalDialog v-if="askingRotation && edgeRotation" title="Apply as G5x rotation?" size="small" fresh-gesture @close="askingRotation = false">
      <p class="rotation-effect">Rotate the {{ wcsName(edgeRotation.wcs) }} XY axes {{ degreesText(edgeRotation.degrees) }} about its stored origin.</p>
      <p v-if="shifted" class="rotation-effect">The current G52/G92 shift stays active.</p>
      <template #footer>
        <button class="btn" type="button" @click="askingRotation = false">Cancel</button>
        <button class="btn primary" type="button" :disabled="!canApplyRotation" @click="applyRotation">Apply rotation</button>
      </template>
    </ModalDialog>
    <NumberDialog
      v-if="askingBoss"
      title="Centre on a boss"
      effect="Probe Z top on the boss and jog over its centre. The cycle probes four sides below Z0 and sets X0 Y0."
      label="Approximate diameter" unit="mm" initial="" action="Probe" @confirm="centreBoss" @close="askingBoss = false"
    />
    <NumberDialog
      v-if="askingGauge"
      title="Calibrate the probe ball"
      :effect="`Probe Z top on the gauge and jog over its centre. The cycle measures both X faces and saves the T${String(tool)} ball diameter (now ${(findTool(tool)?.diameter ?? 0).toFixed(3)} mm).`"
      label="Gauge width" unit="mm" initial="" action="Calibrate" @confirm="calibrate" @close="askingGauge = false"
    />
  </section>
</template>

<style scoped>
.edge-choice { display: grid; grid-template-columns: 1fr 1fr; gap: 6px; }
.edge-choice .btn { min-height: 44px; padding-inline: 5px; }
.rotation-action { flex: none; min-height: 32px; padding-inline: 8px; font-size: 12px; }
.rotation-effect { margin: 0; line-height: 1.5; }
.material-probe { min-width: 0; container-type: inline-size; }
.probe-layout { display: grid; grid-template-columns: minmax(0, 1fr) minmax(186px, 0.82fr); align-items: center; gap: 12px; }
.probe-actions { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 8px; }
.probe-actions button:first-child { grid-column: 1 / -1; }
.probe-actions button { min-height: 56px; min-width: 0; padding-inline: 4px; white-space: nowrap; }
.probe-actions button.corner { min-height: 44px; }
.calibrate { grid-column: 2; align-self: start; min-height: 44px; }
.centres { display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; }
.centres button { min-height: 44px; }
@container (max-width: 390px) {
  .probe-layout { grid-template-columns: minmax(0, 1fr); gap: 8px; }
  .probe-actions { grid-template-columns: repeat(5, minmax(0, 1fr)); }
  .probe-actions button.corner { grid-column: span 1; }
  .probe-actions button:first-child { grid-column: auto; }
}
</style>
