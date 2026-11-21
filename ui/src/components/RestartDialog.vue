<script setup lang="ts">
// Restart a program at a line: the machine plans the state at that line without motion, the
// operator checks it here, and Cycle start runs the tool change, the approach and the program.
import { computed, onBeforeUnmount, ref, shallowRef, watch } from 'vue'
import { findTool, onResult, panel, reported, send } from '../machine'
import { machineBlock } from '../machine-block'
import type { CommandBody, RestartPlan } from '../protocol'
import { formatLength, formatVelocity, toolLabel, velocityUnit, wcsName } from '../units'
import AppIcon from './AppIcon.vue'
import ModalDialog from './ModalDialog.vue'

const props = defineProps<{ line: number; section: string }>()
const emit = defineEmits<{ close: [] }>()

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
    failure.value = 'Waiting for the machine status.'
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
    failure.value = result.reason || 'The machine did not confirm the restart.'
    if (command.type === 'start_restart') plan.value = null
    return
  }
  if (command.type === 'plan_restart') {
    plan.value = result.restart ?? null
    if (plan.value === null) failure.value = 'The machine sent no restart plan.'
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
  if (request.value !== null) return request.value.type === 'plan_restart' ? 'Planning the restart' : 'Starting the restart'
  if (plan.value === null) return failure.value || 'Planning the restart'
  return null
})

const unit = computed(() => panel.unit)
const length = (mm: number): string => `${formatLength(mm, unit.value)} ${unit.value}`
const toolName = (tool: number): string => {
  if (tool === 0) return 'No tool'
  const comment = findTool(tool)?.comment.trim()
  return comment ? `${toolLabel(tool)} · ${comment}` : toolLabel(tool)
}
const loaded = computed(() => {
  const tool = status.value?.tool.in_spindle ?? 0
  return tool === 0 ? 'The spindle is empty. You load the tool at the tool change prompt.'
    : `${toolName(tool)} is loaded. You change it at the tool change prompt.`
})
const spindle = computed(() => {
  const p = plan.value
  if (p === null || p.spindle_direction === 'stopped') return 'Stopped'
  return `${String(Math.round(p.spindle_rpm))} rpm ${p.spindle_direction === 'forward' ? 'forward (M3)' : 'reverse (M4)'}`
})
const coolant = computed(() => {
  const p = plan.value
  if (p === null) return ''
  return [p.flood ? 'Flood' : '', p.mist ? 'Mist' : ''].filter(Boolean).join(' and ') || 'Off'
})
const modes = computed(() => {
  const p = plan.value
  if (p === null) return ''
  return `${p.units === 'inch' ? 'Inch (G20)' : 'mm (G21)'} · ${p.plane} plane · ${p.distance_mode === 'absolute' ? 'Absolute (G90)' : 'Incremental (G91)'}`
})
// The order the machine moves in, so the operator can check the way in is clear.
const steps = computed((): string[] => {
  const p = plan.value
  if (p === null) return []
  const [x, y, z] = p.start
  return [
    `Rapid Z up to ${length(p.clearance_z)}`,
    ...(p.needs_tool_change ? [`Change to ${toolName(p.tool)}, then rapid Z to ${length(p.clearance_z)} with its length`] : []),
    `Rapid to X ${length(x)} Y ${length(y)}`,
    ...(p.spindle_direction === 'stopped' ? [] : [`Start the spindle at ${spindle.value}`]),
    ...(p.flood || p.mist ? [`Turn on ${coolant.value.toLowerCase()} coolant`] : []),
    ...(Math.abs(p.clearance_z - z) > 1e-6
      ? [`Feed Z down to ${length(z)} at ${formatVelocity(p.approach_feed, unit.value)} ${velocityUnit(unit.value)}`] : []),
    `Run the program from line ${String(p.line)}`,
  ]
})
</script>

<template>
  <ModalDialog :title="`Restart at line ${String(line)}`" size="large" fresh-gesture @close="close">
    <p class="context"><span>{{ section }}</span><code v-if="lineText" class="mono">{{ lineText }}</code></p>
    <p v-if="failure" class="failure" role="alert"><AppIcon name="warning" />{{ failure }}</p>
    <p v-else-if="plan === null" class="note" role="status">Planning the restart…</p>
    <div v-if="plan" class="plan">
      <dl class="state">
        <dt>Tool</dt><dd>{{ toolName(plan.tool) }}<small v-if="plan.needs_tool_change">{{ loaded }}</small></dd>
        <dt>Work system</dt><dd>{{ wcsName(plan.wcs) }} · {{ plan.h > 0 ? `G43 H${String(plan.h)}` : 'No length offset (G49)' }}</dd>
        <dt>Spindle</dt><dd>{{ spindle }}</dd>
        <dt>Coolant</dt><dd>{{ coolant }}</dd>
        <dt>Feed</dt><dd>{{ plan.feed > 0 ? `${formatVelocity(plan.feed, unit)} ${velocityUnit(unit)}` : 'Not set' }}</dd>
        <dt>Modes</dt><dd>{{ modes }}</dd>
      </dl>
      <section>
        <h3>Approach</h3>
        <ol class="steps">
          <li v-for="step in steps" :key="step">{{ step }}</li>
        </ol>
        <p class="note">Make sure the way to the start point is clear. Stop halts the restart at any step.</p>
      </section>
    </div>
    <template #footer>
      <button class="btn" type="button" @click="close">Cancel</button>
      <button v-if="failure && request === null" class="btn" type="button" @click="planRestart">Plan again</button>
      <button class="btn primary" type="button" :disabled="startBlock !== null" :title="startBlock ?? 'Run the approach, then the program'" @click="start"><AppIcon name="play" />Cycle start</button>
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
