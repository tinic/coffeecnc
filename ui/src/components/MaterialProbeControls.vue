<script setup lang="ts">
import { onBeforeUnmount, shallowRef } from 'vue'
import { homingActive, manualBlock, onResult, panel, send } from '../machine'
import type { CommandBody, ProbeDirection } from '../protocol'
import { setterState, touchedOff, type SetterStatusLine } from '../setter-state'
import { wcsName } from '../units'

const props = defineProps<{ tool: number }>()
type Request = Extract<CommandBody, { type: 'fixed_setter'; action: 'material' }>
const directions = [
  ['z_top', 'Z top'], ['x_minus', 'X−'], ['x_plus', 'X+'], ['y_minus', 'Y−'], ['y_plus', 'Y+'],
] as const
const pending = shallowRef<Request | null>(null)
const line = shallowRef<SetterStatusLine>({ text: 'Jog near the surface, then choose a probe direction.', level: 'info' })

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
  line.value = { text: `Probing ${directions.find(([id]) => id === direction)?.[1] ?? ''}…`, level: 'busy' }
  send(request)
}

const removeListener = onResult((command, result) => {
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
  line.value = { text: `${wcsName(material.wcs)} ${material.axis}0 set at ${direction === 'z_top' ? 'top' : 'edge'}.`, level: 'done' }
})
onBeforeUnmount(() => {
  if (pending.value) send({ type: 'abort' })
  removeListener()
})
</script>

<template>
  <section class="material-probe" aria-label="Material probe">
    <div class="probe-actions">
      <button
        v-for="[id, title] in directions" :key="id" type="button" class="btn"
        :title="id === 'z_top' ? 'Probe down to find the top surface' : `Move in ${title} to find an edge`"
        @click="probe(id)"
      >
        {{ title }}
      </button>
    </div>
    <p class="hint probe-result" :class="line.level" role="status" :title="line.text">{{ line.text }}</p>
  </section>
</template>

<style scoped>
.material-probe { min-width: 0; }
.probe-actions { display: grid; grid-template-columns: repeat(5, minmax(0, 1fr)); gap: 8px; }
.probe-actions button { min-height: 56px; min-width: 0; padding-inline: 4px; white-space: nowrap; }
.probe-result { height: 32px; line-height: 16px; margin: 8px 0 0; overflow: hidden; display: -webkit-box; -webkit-line-clamp: 2; -webkit-box-orient: vertical; }
.probe-result.blocked { color: var(--stop); }
.probe-result.done { color: var(--go); }
</style>
