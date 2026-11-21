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
import { vTooltip } from '../tooltip'

const props = defineProps<{ tool: number }>()
type Request = Extract<CommandBody, { type: 'fixed_setter'; action: 'material' | 'corner' | 'ball' }>
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
  ? { text: toolBlock.value, level: 'blocked' } : line.value)
const machineReason = computed(() => manualBlock.value ?? (homingActive.value ? homingMessage : null))
let lastMachineReason: string | null = null
watch(machineReason, () => {
  if (lastMachineReason !== null && line.value.text === lastMachineReason) line.value = hint
  lastMachineReason = null
})
let statusRequest: CommandBody | null = null

watch(toolBlock, () => {
  if (line.value.level === 'blocked' && line.value.text.startsWith('Load T')) line.value = hint
})

// Ball calibration: the operator enters the gauge width; the probe starts over the gauge.
const askingGauge = shallowRef(false)
function calibrate(width: number): void {
  askingGauge.value = false
  probe('x_plus', undefined, width)
}

function probe(direction: ProbeDirection, corner?: Corner, gauge?: number): void {
  if (pending.value) return
  const status = panel.status
  const blocked = machineReason.value
  lastMachineReason = blocked
  const [x, y, z] = ['x', 'y', 'z'].map((axis) => status?.axes.find((entry) => entry.axis === axis)?.machine)
  const rpm = status?.spindle.actual_rpm
  let reason = blocked
  if (reason === null && (status?.tool.in_spindle !== props.tool || status.wait.kind === 'tool_change'))
    reason = toolBlock.value ?? `Load T${String(props.tool)} to probe material.`
  if (reason === null && (rpm === null || rpm === undefined || Math.abs(rpm) >= 1 || status?.spindle.direction !== 'stopped'))
    reason = 'Wait for the spindle to stop.'
  if (reason === null && !status?.motion.in_position) reason = 'Wait for the axes to stop.'
  if (reason === null && direction === 'z_top' && !touchedOff(props.tool))
    reason = `Touch off T${String(props.tool)} at the tool setter before probing Z.`
  if (reason === null && corner !== undefined && !touchedOff(props.tool))
    reason = `Touch off T${String(props.tool)} and probe Z top before probing a corner.`
  if (reason === null && gauge !== undefined && !touchedOff(props.tool))
    reason = `Touch off T${String(props.tool)} and probe Z top on the gauge first.`
  if (reason !== null || x === undefined || y === undefined || z === undefined || ![x, y, z].every(Number.isFinite)) {
    line.value = { text: reason ?? 'Waiting for the machine position.', level: 'blocked' }
    return
  }
  const request: Request = gauge !== undefined
    ? { type: 'fixed_setter', action: 'ball', gauge_width: gauge, tool: props.tool, x, y, z }
    : corner === undefined
      ? { type: 'fixed_setter', action: 'material', direction, tool: props.tool, x, y, z }
      : { type: 'fixed_setter', action: 'corner', x_direction: corner.x, y_direction: corner.y, tool: props.tool, x, y, z }
  pending.value = request
  const surface = surfaces.find((entry) => entry.travel === direction)
  line.value = { text: gauge !== undefined ? 'Probing both sides of the gauge…'
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
  if (request.action === 'ball') {
    const measured = result.setter?.ball_diameter
    if (result.outcome !== 'accepted' || measured === undefined) {
      line.value = { text: result.reason || 'The machine did not confirm the ball diameter.', level: 'blocked' }
      return
    }
    if (result.setter) setterState.value = result.setter
    line.value = { text: `T${String(props.tool)} ball measured ${measured.toFixed(3)} mm and saved to the tool table.`, level: 'done' }
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
      <button
        v-tooltip="'Measure the ball on a gauge of known width: probe Z top on the gauge, jog the ball over its middle, then enter the width.'"
        type="button" class="btn quiet calibrate" :class="{ on: pending?.action === 'ball' }" @click="askingGauge = true"
      >
        Calibrate ball…
      </button>
    </div>
    <SetterStatusBand :line="displayLine" />
    <NumberDialog
      v-if="askingGauge"
      title="Calibrate the probe ball"
      :effect="`Probes both X sides of the gauge from beside it and writes the effective diameter of T${String(tool)} (now ${(findTool(tool)?.diameter ?? 0).toFixed(3)} mm) to the tool table.`"
      label="Gauge width" unit="mm" initial="" action="Calibrate" @confirm="calibrate" @close="askingGauge = false"
    />
  </section>
</template>

<style scoped>
.material-probe { min-width: 0; container-type: inline-size; }
.probe-layout { display: grid; grid-template-columns: minmax(0, 1fr) minmax(186px, 0.82fr); align-items: center; gap: 12px; }
.probe-actions { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 8px; }
.probe-actions button:first-child { grid-column: 1 / -1; }
.probe-actions button { min-height: 56px; min-width: 0; padding-inline: 4px; white-space: nowrap; }
.probe-actions button.corner { min-height: 44px; }
.calibrate { grid-column: 2; justify-self: end; min-height: 36px; font-size: 12px; }
@container (max-width: 390px) {
  .probe-layout { grid-template-columns: minmax(0, 1fr); gap: 8px; }
  .probe-actions { grid-template-columns: repeat(5, minmax(0, 1fr)); }
  .probe-actions button.corner { grid-column: span 1; }
  .probe-actions button:first-child { grid-column: auto; }
}
</style>
