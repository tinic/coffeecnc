<script setup lang="ts">
import { computed, onBeforeUnmount, shallowRef, watch, watchEffect } from 'vue'

import { allHomed, homingActive, mdiBlock, onResult, panel, send } from '../machine'
import type { CommandBody, FixedSetterLimits, Vec3 } from '../protocol'
import { toolLabel } from '../units'
import { setterState, setterStatus, type SetterStatusLine } from '../setter-state'

const props = defineProps<{ limits: FixedSetterLimits; showTool?: boolean; inlineStatus?: boolean }>()
type Request = Extract<CommandBody, { type: 'fixed_setter' }>

const state = setterState
const pending = shallowRef<Request | null>(null)
const status = computed(() => panel.status)
const tool = computed(() => status.value?.tool.in_spindle ?? 0)
const teaching = computed(() => tool.value === props.limits.reference_tool)
const running = computed(() => pending.value?.action === 'touch_off')
const position = computed((): Vec3 | null => {
  const [x, y, z] = ['x', 'y', 'z'].map((letter) => status.value?.axes.find((axis) => axis.axis === letter)?.machine)
  return x !== undefined && y !== undefined && z !== undefined && [x, y, z].every(Number.isFinite)
    ? [x, y, z] : null
})
const block = computed(() => {
  if (mdiBlock.value !== null) return mdiBlock.value
  if (homingActive.value) return 'Wait for homing to finish'
  if (position.value === null) return 'Waiting for the machine position'
  const s = status.value
  if (s?.wait.kind === 'tool_change' || tool.value <= 0) return 'Finish loading a tool'
  const rpm = s?.spindle.actual_rpm
  if (rpm === null || rpm === undefined || rpm >= 1 || s?.spindle.direction !== 'stopped') return 'Wait for the spindle to stop'
  if (!s.motion.in_position) return 'Wait for the axes to stop'
  if (!teaching.value && !state.value?.calibration) return `Teach the setter with T${String(props.limits.reference_tool)} first`
  return null
})
// One line the operator reads before pressing: what Touch off does with this tool, or why it can't.
const hint = computed(() => {
  if (!teaching.value) return 'Measures this tool at the setter.'
  return `Jog T${String(props.limits.reference_tool)} over the setter, then Touch off.`
})
const summary = computed(() => {
  const result = state.value?.measurement
  if (result?.tool !== tool.value) return ''
  // The length itself is in the tool table: only confirm what happened.
  return result.calibration ? 'Setter taught' : `${toolLabel(result.tool)} measured`
})

const line = computed((): SetterStatusLine => {
  if (running.value) return { text: 'Touching off…', level: 'busy' }
  if (summary.value) return { text: summary.value, level: 'done' }
  return block.value === null ? { text: hint.value, level: 'info' } : { text: block.value, level: 'blocked' }
})
watchEffect(() => { setterStatus.value = line.value })

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
      <strong v-if="showTool && tool !== 0" class="mono">{{ toolLabel(tool) }}</strong>
      <button v-if="running" class="btn danger" type="button" @click="send({ type: 'abort' })">Stop setter</button>
      <button v-else class="btn primary" type="button" :disabled="!!pending || block !== null" @click="touchOff">Touch off</button>
    </div>
    <p v-if="inlineStatus" class="hint setter-result" :class="line.level" aria-live="polite">{{ line.text }}</p>
  </div>
</template>

<style scoped>
/* One row, fixed size: status text left of the button, so the changing text never moves the
   buttons around it and the row lines up with the other header buttons. */
.setter-controls { flex: none; display: flex; flex-direction: row-reverse; align-items: center; gap: 10px; }
.setter-action { display: flex; align-items: center; gap: 8px; }
.setter-action button { width: 130px; min-height: 44px; white-space: nowrap; }
.setter-result {
  width: 260px; margin: 0; text-align: right; font-size: 12px; line-height: 1.3;
  white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
}
.setter-result.blocked { color: var(--stop); }
</style>
