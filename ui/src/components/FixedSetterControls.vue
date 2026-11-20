<script setup lang="ts">
import { computed, onBeforeUnmount, shallowRef, watch } from 'vue'

import { allHomed, homingActive, mdiBlock, onResult, panel, send } from '../machine'
import type { CommandBody, FixedSetterLimits, SetterState, Vec3 } from '../protocol'
import { vTooltip } from '../tooltip'
import { formatLength } from '../units'

const props = defineProps<{ limits: FixedSetterLimits; showTool?: boolean }>()
type Request = Extract<CommandBody, { type: 'fixed_setter' }>

const state = shallowRef<SetterState | null>(null)
const pending = shallowRef<Request | null>(null)
const status = computed(() => panel.status)
const tool = computed(() => status.value?.tool.in_spindle ?? 0)
const teaching = computed(() => tool.value === props.limits.reference_tool)
const running = computed(() => pending.value?.action === 'touch_off')
const unit = computed(() => panel.unit === 'inch' ? 'in' : 'mm')
const length = (mm: number): string => `${formatLength(mm, panel.unit)} ${unit.value}`
const position = computed((): Vec3 | null => {
  const [x, y, z] = ['x', 'y', 'z'].map((letter) => status.value?.axes.find((axis) => axis.axis === letter)?.machine)
  return x !== undefined && y !== undefined && z !== undefined && [x, y, z].every(Number.isFinite)
    ? [x, y, z] : null
})
const block = computed(() => {
  if (mdiBlock.value !== null) return mdiBlock.value
  if (!allHomed.value || position.value === null) return 'Home XYZ first'
  const s = status.value
  if (s?.wait.kind === 'tool_change' || tool.value <= 0) return 'Finish loading a tool'
  const rpm = s?.spindle.actual_rpm
  if (rpm === null || rpm === undefined || rpm >= 1 || s?.spindle.direction !== 'stopped') return 'Wait for the spindle to stop'
  if (!s.motion.in_position) return 'Wait for the axes to stop'
  if (!teaching.value && !state.value?.calibration) return `Teach the setter with T${String(props.limits.reference_tool)} first`
  return null
})
const hint = computed(() => {
  if (!teaching.value) return 'Raises Z to clearance, moves XY to the setter, and saves the measured length.'
  const center = `Center T${String(props.limits.reference_tool)} over the setter, within ${length(props.limits.travel_mm)} of contact.`
  const clearance = props.limits.clearance_mm ?? 0
  return clearance > 0
    ? `${center} Verify the lift of ${length(clearance)} from contact and the XY travel clearance.`
    : `${center} Teach a Z height that clears the work, clamps and longest tool along the travel path.`
})
const summary = computed(() => {
  const result = state.value?.measurement
  if (result?.tool !== tool.value) return ''
  const headline = result.calibration ? 'Setter taught' : `T${String(result.tool)} length ${length(result.value)}`
  return result.stopping_distance === undefined ? headline : `${headline} · Z after contact ${length(result.stopping_distance)}`
})

function request(command: Request): void {
  pending.value = command
  send(command)
}

function refresh(): void {
  if (running.value) return
  state.value = null
  pending.value = null
  if (panel.link === 'live') request({ type: 'fixed_setter', action: 'status' })
}

function touchOff(): void {
  const xyz = position.value
  if (pending.value || block.value || xyz === null) return
  request({ type: 'fixed_setter', action: 'touch_off', tool: tool.value, x: xyz[0], y: xyz[1], z: xyz[2] })
}

const removeListener = onResult((command, result) => {
  if (command !== pending.value) return
  const motion = running.value
  pending.value = null
  if (result.outcome !== 'accepted') {
    if (motion && result.outcome === 'timed_out') send({ type: 'abort' })
    if (motion) refresh()
    return
  }
  state.value = result.setter ?? null
})
watch([(): unknown => panel.hello, (): string => panel.link, (): string | undefined => status.value?.machine, allHomed, homingActive],
  refresh, { immediate: true })
onBeforeUnmount(() => {
  if (running.value) send({ type: 'abort' })
  removeListener()
})
</script>

<template>
  <div class="setter-controls">
    <div class="setter-action">
      <span v-if="showTool && tool !== 0" class="mono">T{{ tool }}</span>
      <button v-if="running" class="btn danger" type="button" @click="send({ type: 'abort' })">Stop setter</button>
      <button v-else v-tooltip="block ?? hint" class="btn" type="button" :disabled="!!pending || block !== null" @click="touchOff">
        {{ teaching ? 'Teach setter' : 'Touch off at setter' }}
      </button>
    </div>
    <output v-if="summary && !running" class="hint setter-result" aria-live="polite">{{ summary }}</output>
  </div>
</template>

<style scoped>
.setter-controls { min-width: 0; }
.setter-action { display: flex; align-items: center; gap: 8px; }
.setter-action button { min-height: 44px; white-space: nowrap; }
.setter-result { display: block; max-width: 185px; line-height: 1.35; }
</style>
