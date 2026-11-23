<script setup lang="ts">
// Restart a program at a line: the machine plans the state at that line without motion, the
// operator checks it here, and Cycle start runs the tool change, the approach and the program.
import { computed, onBeforeUnmount, ref, shallowRef, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { findTool, onResult, panel, reported, send } from '../machine'
import { machineBlock } from '../machine-block'
import type { CommandBody, RestartPlan } from '../protocol'
import { formatLength, formatVelocity, toolLabel, velocityUnit, wcsName } from '../units'
import AppIcon from './AppIcon.vue'
import { vTooltip } from '../tooltip'
import ModalDialog from './ModalDialog.vue'
import { translateMessage } from '../i18n/messages'

const props = defineProps<{ line: number; section: string }>()
const emit = defineEmits<{ close: [] }>()
const { t } = useI18n()

const status = reported
const plan = shallowRef<RestartPlan | null>(null)
const request = shallowRef<CommandBody | null>(null)
const failure = ref('')
const started = ref(false)
const lineText = computed(() => panel.program?.lines[props.line - 1]?.trim() ?? '')

function planRestart(): void {
  const context = status.value?.preview_context
  plan.value = null
  failure.value = ''
  if (context === undefined) {
    failure.value = t('program.restartDialog.waitingStatus')
    return
  }
  const command: CommandBody = { type: 'plan_restart', line: props.line, preview_context: context }
  request.value = command
  send(command)
}

/** Closing while the start is unconfirmed stops whatever the machine already began. */
function close(): void {
  if (request.value?.type === 'start_restart') send({ type: 'abort' })
  emit('close')
}

function start(): void {
  if (plan.value === null || startBlock.value !== null) return
  failure.value = ''
  const command: CommandBody = { type: 'start_restart', plan_id: plan.value.plan_id }
  request.value = command
  send(command)
}

const removeListener = onResult((command, result) => {
  if (command !== request.value) return
  request.value = null
  if (result.outcome !== 'accepted') {
    failure.value = result.reason || t('program.restartDialog.notConfirmed')
    if (command.type === 'start_restart') {
      plan.value = null
      // No answer does not mean nothing moves: stop what the machine may still be doing.
      if (result.outcome === 'timed_out') {
        send({ type: 'abort' })
        failure.value = t('program.restartDialog.notConfirmedStopped')
      }
    }
    return
  }
  if (command.type === 'plan_restart') {
    plan.value = result.restart ?? null
    if (plan.value === null) failure.value = t('program.restartDialog.noPlan')
  } else {
    started.value = true
    emit('close')
  }
})

// A plan holds for the program and machine state it was made for; plan again when they change.
watch(() => status.value?.preview_context, (context, before) => {
  if (context !== before && !started.value && request.value?.type !== 'start_restart') planRestart()
})
planRestart()
onBeforeUnmount(removeListener)

const startBlock = computed((): string | null => {
  const shared = machineBlock(panel)
  if (shared !== null) return shared
  if (request.value !== null) return request.value.type === 'plan_restart' ? t('program.restartDialog.planning') : t('program.restartDialog.starting')
  if (plan.value === null) return failure.value ? translateMessage(failure.value) : t('program.restartDialog.planning')
  return null
})

const unit = computed(() => panel.unit)
const length = (mm: number): string => `${formatLength(mm, unit.value)} ${unit.value}`
const toolName = (tool: number): string => {
  if (tool === 0) return t('program.restartDialog.noTool')
  const comment = findTool(tool)?.comment.trim()
  return comment ? `${toolLabel(tool)} · ${comment}` : toolLabel(tool)
}
const loaded = computed(() => {
  const tool = status.value?.tool.in_spindle ?? 0
  return tool === 0 ? t('program.restartDialog.spindleEmpty')
    : t('program.restartDialog.toolLoaded', { tool: toolName(tool) })
})
const spindle = computed(() => {
  const p = plan.value
  if (p === null || p.spindle_direction === 'stopped') return t('program.restartDialog.stopped')
  const rpm = String(Math.round(p.spindle_rpm))
  return p.spindle_direction === 'forward' ? t('program.restartDialog.forward', { rpm }) : t('program.restartDialog.reverse', { rpm })
})
const coolant = computed(() => {
  const p = plan.value
  if (p === null) return ''
  return p.flood && p.mist ? t('program.restartDialog.floodAndMist') : p.flood ? t('program.restartDialog.flood')
    : p.mist ? t('program.restartDialog.mist') : t('common.off')
})
const modes = computed(() => {
  const p = plan.value
  if (p === null) return ''
  return t('program.restartDialog.modes', {
    units: p.units === 'inch' ? t('program.restartDialog.inch') : 'mm (G21)', plane: p.plane,
    distance: p.distance_mode === 'absolute' ? t('program.restartDialog.absolute') : t('program.restartDialog.incremental'),
  })
})
// The order the machine moves in, so the operator can check the way in is clear.
const steps = computed((): string[] => {
  const p = plan.value
  if (p === null) return []
  const [x, y, z] = p.start
  return [
    t('program.restartDialog.stepRapidUp', { z: length(p.clearance_z) }),
    ...(p.needs_tool_change ? [t('program.restartDialog.stepChange', { tool: toolName(p.tool), z: length(p.clearance_z) })] : []),
    t('program.restartDialog.stepRapidXY', { x: length(x), y: length(y) }),
    ...(p.spindle_direction === 'stopped' ? [] : [t('program.restartDialog.stepSpindle', { spindle: spindle.value })]),
    ...(p.flood && p.mist ? [t('program.restartDialog.stepFloodAndMist')] : p.flood ? [t('program.restartDialog.stepFlood')]
      : p.mist ? [t('program.restartDialog.stepMist')] : []),
    ...(Math.abs(p.clearance_z - z) > 1e-6
      ? [t('program.restartDialog.stepFeedDown', { z: length(z), feed: `${formatVelocity(p.approach_feed, unit.value)} ${velocityUnit(unit.value)}` })] : []),
    t('program.restartDialog.stepRun', { line: p.line }),
  ]
})
</script>

<template>
  <ModalDialog :title="t('program.restartDialog.title', { line })" size="large" fresh-gesture @close="close">
    <p class="context"><span>{{ section }}</span><code v-if="lineText" class="mono">{{ lineText }}</code></p>
    <p v-if="failure" class="failure" role="alert"><AppIcon name="warning" />{{ translateMessage(failure) }}</p>
    <p v-else-if="plan === null" class="note" role="status">{{ t('program.restartDialog.planningRestart') }}</p>
    <div v-if="plan" class="plan">
      <dl class="state">
        <dt>{{ t('program.restartDialog.tool') }}</dt><dd>{{ toolName(plan.tool) }}<small v-if="plan.needs_tool_change">{{ loaded }}</small></dd>
        <dt>{{ t('program.restartDialog.workSystem') }}</dt><dd>{{ wcsName(plan.wcs) }} · {{ plan.h > 0 ? `G43 H${String(plan.h)}` : t('program.restartDialog.noLengthOffset') }}</dd>
        <dt>{{ t('program.restartDialog.spindle') }}</dt><dd>{{ spindle }}</dd>
        <dt>{{ t('program.restartDialog.coolant') }}</dt><dd>{{ coolant }}</dd>
        <dt>{{ t('program.restartDialog.feed') }}</dt><dd>{{ plan.feed > 0 ? `${formatVelocity(plan.feed, unit)} ${velocityUnit(unit)}` : t('program.restartDialog.notSet') }}</dd>
        <dt>{{ t('program.restartDialog.modesLabel') }}</dt><dd>{{ modes }}</dd>
      </dl>
      <section>
        <h3>{{ t('program.restartDialog.approach') }}</h3>
        <ol class="steps">
          <li v-for="step in steps" :key="step">{{ step }}</li>
        </ol>
        <p class="note">{{ t('program.restartDialog.clearWay') }}</p>
      </section>
    </div>
    <template #footer>
      <button class="btn" type="button" @click="close">{{ request?.type === 'start_restart' ? t('program.restartDialog.stop') : t('common.cancel') }}</button>
      <button v-if="failure && request === null" class="btn" type="button" @click="planRestart">{{ t('program.restartDialog.planAgain') }}</button>
      <button
        v-tooltip="request ? undefined : startBlock ?? t('program.restartDialog.startTooltip')"
        class="btn primary" type="button" :disabled="startBlock !== null" @click="start"
      >
        <AppIcon name="play" />{{ request?.type === 'start_restart' ? t('program.restartDialog.starting') : t('program.restartDialog.cycleStart') }}
      </button>
    </template>
  </ModalDialog>
</template>

<style scoped>
.context { display: flex; flex-wrap: wrap; align-items: baseline; gap: 10px; margin: 0 0 14px; color: var(--text-dim); font-size: 13px; }
.context code { padding: 2px 8px; border-radius: 6px; background: var(--bg-sunken); color: var(--text); font-size: 13px; }
.plan { display: grid; grid-template-columns: minmax(0, 1fr) minmax(0, 1fr); gap: 28px; }
.state { display: grid; grid-template-columns: max-content 1fr; align-content: start; gap: 8px 18px; margin: 0; font-size: 13px; }
.state dt { color: var(--text-dim); }
.state dd { margin: 0; }
.state small { display: block; margin-top: 3px; color: var(--text-dim); font-size: 12px; }
h3 { margin: 0 0 8px; font-size: 12px; font-weight: 600; letter-spacing: .6px; text-transform: uppercase; color: var(--text-dim); }
.steps { margin: 0 0 14px; padding-left: 22px; font-size: 13px; line-height: 1.7; }
.note { margin: 0; color: var(--text-dim); font-size: 12px; }
.failure { display: flex; align-items: center; gap: 8px; margin: 0 0 12px; padding: 10px 12px; border: 1px solid var(--stop); border-radius: 9px; background: var(--stop-soft); color: var(--stop); font-size: 13px; }
.failure :deep(svg) { width: 16px; flex: none; }
</style>
