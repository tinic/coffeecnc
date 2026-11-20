<script setup lang="ts">
import { computed, onBeforeUnmount, shallowRef, watch } from 'vue'

import { allHomed, homingActive, mdiBlock, onResult, panel, send } from '../machine'
import type { CommandBody, FixedSetterLimits, SetterState, Vec3 } from '../protocol'

const props = defineProps<{ limits: FixedSetterLimits; showTool?: boolean }>()
type Request = Extract<CommandBody, { type: 'fixed_setter' }>

const state = shallowRef<SetterState | null>(null)
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
  if (!teaching.value) return 'Goes to the setter and measures this tool.'
  return `Jog T${String(props.limits.reference_tool)} over the setter, then Touch off.`
})
const summary = computed(() => {
  const result = state.value?.measurement
  if (result?.tool !== tool.value) return ''
  // The length itself is in the tool table: only confirm what happened.
  return result.calibration ? 'Setter taught' : `T${String(result.tool)} measured`
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
      <button v-else class="btn primary" type="button" :disabled="!!pending || block !== null" @click="touchOff">Touch off</button>
    </div>
    <output v-if="summary && !running" class="hint setter-result" aria-live="polite">{{ summary }}</output>
    <p v-else class="hint setter-result" :class="{ blocked: block !== null }">{{ running ? 'Touching off…' : block ?? hint }}</p>
  </div>
</template>

<style scoped>
/* Fixed size: the changing status text must not move the buttons around it. */
.setter-controls { flex: none; width: 240px; }
.setter-action { display: flex; align-items: center; gap: 8px; }
.setter-action button { flex: 1; min-height: 44px; white-space: nowrap; }
.setter-result {
  display: -webkit-box; -webkit-box-orient: vertical; -webkit-line-clamp: 2; overflow: hidden;
  height: 2.7em; margin: 4px 0 0; line-height: 1.35;
}
.setter-result.blocked { color: var(--warn, #b45309); }
</style>
