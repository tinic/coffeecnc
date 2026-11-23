<script setup lang="ts">
import { computed, onBeforeUnmount, shallowRef, watch, watchEffect } from 'vue'
import { useI18n } from 'vue-i18n'
import { translateMessage } from '../i18n/messages'

import { allHomed, findTool, homingActive, mdiBlock, onResult, panel, send } from '../machine'
import { useCardFeedback } from '../card-feedback'
import { machineBlock } from '../machine-block'
import type { CommandBody, FixedSetterLimits, Vec3 } from '../protocol'
import { formatLength, toolLabel } from '../units'
import { setterState, setterStatus, touchedOff, type SetterStatusLine } from '../setter-state'
import ProbeGuideDialog from './ProbeGuideDialog.vue'
import type { ProbeGuideTarget } from '../probe-guide'
import { touchProbeLoaded } from '../tool-models'

const props = defineProps<{ limits: FixedSetterLimits }>()
const { t } = useI18n()
type Request = Extract<CommandBody, { type: 'fixed_setter' }>

const state = setterState
const pending = shallowRef<Request | null>(null)
const status = computed(() => panel.status)
const tool = computed(() => status.value?.tool.in_spindle ?? 0)
// The reference tool always teaches. Before the setter is taught (e.g. after a restart), any
// tool whose saved length is still valid teaches it too (turo: T99 can act like T98).
const teaching = computed(() => tool.value === props.limits.reference_tool ||
  (!state.value?.calibration && tool.value > 0 && touchedOff(tool.value)))
const running = computed(() => pending.value?.action === 'touch_off' || pending.value?.action === 'check')
const position = computed((): Vec3 | null => {
  const [x, y, z] = ['x', 'y', 'z'].map((letter) => status.value?.axes.find((axis) => axis.axis === letter)?.machine)
  return x !== undefined && y !== undefined && z !== undefined && [x, y, z].every(Number.isFinite)
    ? [x, y, z] : null
})
const machineReason = computed(() => machineBlock(panel))
const localBlock = computed(() => {
  if (machineReason.value !== null) return null
  if (mdiBlock.value !== null) return mdiBlock.value
  if (position.value === null) return t('tools.setter.waitingPosition')
  const s = status.value
  if (s?.wait.kind === 'tool_change' || tool.value <= 0) return t('tools.setter.finishLoading')
  const rpm = s?.spindle.actual_rpm
  if (rpm === null || rpm === undefined || rpm >= 1 || s?.spindle.direction !== 'stopped') return t('tools.setter.waitSpindle')
  if (!s.motion.in_position) return t('tools.setter.waitAxes')
  return null
})
const teachFirst = computed(() => t('tools.setter.teachFirst', { number: props.limits.reference_tool }))
const checkBlock = computed(() => {
  if (touchProbeLoaded.value) return t('tools.setter.loadCutter')
  if (!state.value?.calibration) return teachFirst.value
  const loaded = findTool(tool.value)
  if (loaded === null || (loaded.length === 0 && !touchedOff(tool.value))) return t('tools.setter.setLengthFirst')
  return null
})
const { line: feedback, allow } = useCardFeedback(
  () => '',
  () => machineReason.value !== null ? null
    : localBlock.value ?? (!teaching.value && !state.value?.calibration ? teachFirst.value : null),
  [machineReason, localBlock, checkBlock, tool, pending],
)
const error = shallowRef<(() => string) | null>(null)
// An idle update can arrive alongside a failed cycle result. Keep the result through that
// transition, and clear it when the machine or loaded tool changes.
watch([(): unknown => panel.hello, (): string => panel.link, (): string | undefined => status.value?.machine,
  tool, allHomed, homingActive], () => { error.value = null })
const summary = computed(() => {
  const result = state.value?.measurement
  if (result?.tool !== tool.value) return ''
  if (result.check_tolerance_mm !== undefined) {
    const loaded = findTool(result.tool)
    if (loaded === null || Math.abs(loaded.length - result.previous_length) > 1e-6) return ''
    const difference = formatLength(Math.abs(result.value - result.previous_length), panel.unit)
    return t('tools.setter.passed', { tool: toolLabel(result.tool), difference, unit: panel.unit === 'inch' ? 'in' : 'mm' })
  }
  // The length itself is in the tool table: only confirm what happened.
  return result.calibration ? t('tools.setter.taught') : t('tools.setter.measured', { tool: toolLabel(result.tool) })
})

const line = computed((): SetterStatusLine => {
  if (running.value) return { text: pending.value?.action === 'check' ? t('tools.setter.checking') : t('tools.setter.touching'), level: 'busy' }
  if (feedback.value.level === 'blocked') return feedback.value
  if (error.value !== null) return { text: error.value(), level: 'blocked' }
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

function startBlock(action: 'touch_off' | 'check'): string | null {
  const operation = action === 'check' ? checkBlock.value : null
  return machineReason.value ?? (pending.value ? t('tools.setter.waitingStatus') : localBlock.value) ?? operation ??
    (!teaching.value && !state.value?.calibration ? teachFirst.value : null)
}
const guide = shallowRef<Extract<ProbeGuideTarget, { kind: 'teach' | 'measure' | 'check' }> | null>(null)
// A slow render can delay status delivery. Keep the instructions visible and wait for
// current controller and setter data before enabling confirmation.
watch([tool, (): unknown => panel.hello], () => { guide.value = null })
const guideBlocked = computed((): string | null => {
  const target = guide.value
  if (target === null) return null
  const block = startBlock(target.kind === 'check' ? 'check' : 'touch_off')
  if (block !== null) return block
  return target.kind !== 'check' && (target.kind === 'teach') !== teaching.value
    ? t('tools.setter.setupChanged') : null
})
function openCheck(): void {
  error.value = null
  if (allow(startBlock('check'))) guide.value = { kind: 'check' }
}
function openTouchOff(): void {
  error.value = null
  if (allow(startBlock('touch_off'))) guide.value = { kind: teaching.value ? 'teach' : 'measure' }
}
function startGuide(): void {
  const target = guide.value
  const xyz = position.value
  if (target === null || !allow(guideBlocked.value) || xyz === null) return
  guide.value = null
  error.value = null
  request({ type: 'fixed_setter', action: target.kind === 'check' ? 'check' : 'touch_off',
    tool: tool.value, x: xyz[0], y: xyz[1], z: xyz[2] })
}

const removeListener = onResult((command, result) => {
  if (command !== pending.value) return
  const motion = running.value
  pending.value = null
  if (result.outcome !== 'accepted') {
    if (motion && result.outcome === 'timed_out') send({ type: 'abort' })
    if (motion) {
      error.value = (): string => translateMessage(result.reason, result.refusal) || (command.action === 'check'
        ? t('tools.setter.checkNotConfirmed') : t('tools.setter.measureNotConfirmed'))
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
      <button v-if="running" class="btn danger" type="button" @click="send({ type: 'abort' })">{{ t('tools.setter.stop') }}</button>
      <template v-else>
        <button v-if="limits.check_tolerance_mm !== undefined" class="btn check-length" type="button" @click="openCheck">{{ t('tools.setter.checkLength') }}</button>
        <button class="btn primary" type="button" @click="openTouchOff">{{ t('tools.setter.touchOff') }}</button>
      </template>
    </div>
  </div>
  <ProbeGuideDialog
    v-if="guide" :target="guide" :limits="limits" :tool="tool" :ruby="tool === panel.hello?.touch_probe_tool"
    :start-blocked="guideBlocked" @start="startGuide" @close="guide = null"
  />
</template>

<style scoped>
.setter-controls { flex: none; display: flex; flex-direction: row-reverse; align-items: center; gap: 10px; }
.setter-action { display: flex; align-items: center; gap: 8px; }
.setter-action button { width: 130px; min-height: 44px; white-space: nowrap; }
.setter-action .check-length { width: 116px; }
</style>
