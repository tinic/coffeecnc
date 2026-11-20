<script setup lang="ts">
import { onBeforeUnmount, shallowRef, watch } from 'vue'
import { allHomed, homingActive, manualBlock, onResult, panel, send } from '../machine'
import type { CommandBody, ProbeDirection } from '../protocol'
import { setterState, touchedOff, type SetterStatusLine } from '../setter-state'
import { wcsName } from '../units'
import SetterStatusBand from './SetterStatusBand.vue'
import WorkpieceProbeDiagram from './WorkpieceProbeDiagram.vue'
import { vTooltip } from '../tooltip'

const props = defineProps<{ tool: number }>()
type Request = Extract<CommandBody, { type: 'fixed_setter'; action: 'material' }>
// The button names the part face. The protocol still specifies the tool's travel.
const surfaces = [
  { label: 'Z top', travel: 'z_top', surface: 'top surface', approach: '−Z' },
  { label: 'X−', travel: 'x_plus', surface: 'left edge', approach: '+X' },
  { label: 'X+', travel: 'x_minus', surface: 'right edge', approach: '−X' },
  { label: 'Y−', travel: 'y_plus', surface: 'front edge', approach: '+Y' },
  { label: 'Y+', travel: 'y_minus', surface: 'back edge', approach: '−Y' },
] as const
const pending = shallowRef<Request | null>(null)
const line = shallowRef<SetterStatusLine>({ text: 'Jog outside the part, then choose the face to probe.', level: 'info' })
let statusRequest: CommandBody | null = null

function probe(direction: ProbeDirection): void {
  if (pending.value) return
  const status = panel.status
  const blocked = manualBlock.value ?? (homingActive.value ? 'Wait for homing to finish.' : null)
  const [x, y, z] = ['x', 'y', 'z'].map((axis) => status?.axes.find((entry) => entry.axis === axis)?.machine)
  const rpm = status?.spindle.actual_rpm
  let reason = blocked
  if (reason === null && (status?.tool.in_spindle !== props.tool || status.wait.kind === 'tool_change'))
    reason = `Load T${String(props.tool)} to probe material.`
  if (reason === null && (rpm === null || rpm === undefined || Math.abs(rpm) >= 1 || status?.spindle.direction !== 'stopped'))
    reason = 'Wait for the spindle to stop.'
  if (reason === null && !status?.motion.in_position) reason = 'Wait for the axes to stop.'
  if (reason === null && direction === 'z_top' && !touchedOff(props.tool))
    reason = `Touch off T${String(props.tool)} at the tool setter before probing Z.`
  if (reason !== null || x === undefined || y === undefined || z === undefined || ![x, y, z].every(Number.isFinite)) {
    line.value = { text: reason ?? 'Waiting for the machine position.', level: 'blocked' }
    return
  }
  const request: Request = { type: 'fixed_setter', action: 'material', direction, tool: props.tool, x, y, z }
  pending.value = request
  const surface = surfaces.find((entry) => entry.travel === direction)
  line.value = { text: `Probing ${surface?.surface ?? 'surface'}…`, level: 'busy' }
  send(request)
}

const removeListener = onResult((command, result) => {
  if (command === statusRequest) {
    statusRequest = null
    if (result.outcome === 'accepted') setterState.value = result.setter ?? null
    return
  }
  if (command !== pending.value) return
  const direction = pending.value.direction
  pending.value = null
  if (result.outcome === 'timed_out') send({ type: 'abort' })
  const material = result.setter?.material
  if (result.outcome !== 'accepted' || !material) {
    line.value = { text: result.reason || 'The machine did not confirm the work origin.', level: 'blocked' }
    return
  }
  if (result.setter) setterState.value = result.setter
  const surface = surfaces.find((entry) => entry.travel === direction)
  const face = direction === 'z_top' ? 'the top surface' : `the ${surface?.label ?? ''} ${surface?.surface ?? 'edge'}`
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
          type="button" class="btn" :class="{ on: pending?.direction === face.travel }"
          @click="probe(face.travel)"
        >
          {{ face.label }}
        </button>
      </div>
      <WorkpieceProbeDiagram :travel="pending?.direction ?? null" />
    </div>
    <SetterStatusBand :line="line" />
  </section>
</template>

<style scoped>
.material-probe { min-width: 0; container-type: inline-size; }
.probe-layout { display: grid; grid-template-columns: minmax(0, 1fr) minmax(186px, 0.82fr); align-items: center; gap: 12px; }
.probe-actions { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 8px; }
.probe-actions button:first-child { grid-column: 1 / -1; }
.probe-actions button { min-height: 56px; min-width: 0; padding-inline: 4px; white-space: nowrap; }
@container (max-width: 390px) {
  .probe-layout { grid-template-columns: minmax(0, 1fr); gap: 8px; }
  .probe-actions { grid-template-columns: repeat(5, minmax(0, 1fr)); }
  .probe-actions button:first-child { grid-column: auto; }
}
</style>
