<script setup lang="ts">
import { computed, onBeforeUnmount, shallowRef, watch, watchEffect } from 'vue'

import { allHomed, homingActive, mdiBlock, onResult, panel, send } from '../machine'
import { useCardFeedback } from '../card-feedback'
import { machineBlock } from '../machine-block'
import type { CommandBody, FixedSetterLimits, Vec3 } from '../protocol'
import { toolLabel } from '../units'
import { setterState, setterStatus, touchedOff, type SetterStatusLine } from '../setter-state'
import ProbeGuideDialog from './ProbeGuideDialog.vue'
import type { ProbeGuideTarget } from '../probe-guide'

const props = defineProps<{ limits: FixedSetterLimits }>()
type Request = Extract<CommandBody, { type: 'fixed_setter' }>

const state = setterState
const pending = shallowRef<Request | null>(null)
const status = computed(() => panel.status)
const tool = computed(() => status.value?.tool.in_spindle ?? 0)
// The reference tool always teaches. Before the setter is taught (e.g. after a restart), any
// tool whose saved length is still valid teaches it too (turo: T99 can act like T98).
const teaching = computed(() => tool.value === props.limits.reference_tool ||
  (!state.value?.calibration && tool.value > 0 && touchedOff(tool.value)))
const running = computed(() => pending.value?.action === 'touch_off')
const position = computed((): Vec3 | null => {
  const [x, y, z] = ['x', 'y', 'z'].map((letter) => status.value?.axes.find((axis) => axis.axis === letter)?.machine)
  return x !== undefined && y !== undefined && z !== undefined && [x, y, z].every(Number.isFinite)
    ? [x, y, z] : null
})
const machineReason = computed(() => machineBlock(panel))
const localBlock = computed(() => {
  if (machineReason.value !== null) return null
  if (mdiBlock.value !== null) return mdiBlock.value
  if (position.value === null) return 'Waiting for the machine position'
  const s = status.value
  if (s?.wait.kind === 'tool_change' || tool.value <= 0) return 'Finish loading a tool'
  const rpm = s?.spindle.actual_rpm
  if (rpm === null || rpm === undefined || rpm >= 1 || s?.spindle.direction !== 'stopped') return 'Wait for the spindle to stop'
  if (!s.motion.in_position) return 'Wait for the axes to stop'
  if (!teaching.value && !state.value?.calibration) return `Teach the setter with T${String(props.limits.reference_tool)} or a touched-off tool first`
  return null
})
const { line: feedback, allow } = useCardFeedback(
  () => '',
  () => localBlock.value,
  [machineReason, localBlock, tool, pending],
)
const error = shallowRef<string | null>(null)
watch([machineReason, tool], () => { error.value = null })
const summary = computed(() => {
  const result = state.value?.measurement
  if (result?.tool !== tool.value) return ''
  // The length itself is in the tool table: only confirm what happened.
  return result.calibration ? 'Setter taught' : `${toolLabel(result.tool)} measured`
})

const line = computed((): SetterStatusLine => {
  if (running.value) return { text: 'Touching off…', level: 'busy' }
  if (feedback.value.level === 'blocked') return feedback.value
  if (error.value !== null) return { text: error.value, level: 'blocked' }
  if (summary.value) return { text: summary.value, level: 'done' }
  return feedback.value
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
  error.value = null
  const reason = machineReason.value ?? (pending.value ? 'Waiting for setter status' : localBlock.value)
  if (!allow(reason) || xyz === null) return
  request({ type: 'fixed_setter', action: 'touch_off', tool: tool.value, x: xyz[0], y: xyz[1], z: xyz[2] })
}
const guide = shallowRef<ProbeGuideTarget | null>(null)
watch([tool, teaching, (): unknown => panel.hello, (): string => panel.link], () => { guide.value = null })
function startGuide(): void {
  guide.value = null
  touchOff()
}

const removeListener = onResult((command, result) => {
  if (command !== pending.value) return
  const motion = running.value
  pending.value = null
  if (result.outcome !== 'accepted') {
    if (motion && result.outcome === 'timed_out') send({ type: 'abort' })
    if (motion) {
      error.value = result.reason || 'The machine did not confirm the tool measurement.'
      refresh()
    }
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
      <button v-if="running" class="btn danger" type="button" @click="send({ type: 'abort' })">Stop setter</button>
      <button v-else class="btn primary" type="button" @click="guide = { kind: teaching ? 'teach' : 'measure' }">Touch off</button>
    </div>
  </div>
  <ProbeGuideDialog v-if="guide" :target="guide" :limits="limits" :tool="tool" @start="startGuide" @close="guide = null" />
</template>

<style scoped>
.setter-controls { flex: none; display: flex; flex-direction: row-reverse; align-items: center; gap: 10px; }
.setter-action { display: flex; align-items: center; gap: 8px; }
.setter-action button { width: 130px; min-height: 44px; white-space: nowrap; }
</style>
