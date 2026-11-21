<script setup lang="ts">
import { vTooltip } from '../tooltip'
import { vPopover } from '../popover'
// Keep Stop available in every workspace. Touch-off needs no program setup controls.
import { computed, onBeforeUnmount, ref, shallowRef, watch } from 'vue'

import { allHomed, homingRequired, linkBlock, live, onResult, panel, reported, send } from '../machine'
import { connectionBlock, interpreterBlock, machineBlock } from '../machine-block'
import { currentProgramPreview, describeProgramCheck } from '../program-check'
import { breakpointSelection, selectedBreakpoint, type BreakpointSelection } from '../program-breakpoint'
import type { CommandBody, LengthUnit } from '../protocol'
import { formatLength } from '../units'
import { touchProbeLoaded } from '../tool-models'

import AppIcon from './AppIcon.vue'
import OverrideControl from './OverrideControl.vue'
import ProgramCheckDialog from './ProgramCheckDialog.vue'

const status = reported
const checkedPreview = computed(() => currentProgramPreview(panel.preview ?? null, status.value))
const check = computed(() => describeProgramCheck(checkedPreview.value, panel.unit))
const airZ = computed(() => status.value?.program.air_z ?? 0)
const airOwned = computed(() => status.value?.program.air_offset_owned === true)
const activeBreakpoint = computed(() => status.value?.program.breakpoint)
const breakpoint = computed(() => airZ.value || activeBreakpoint.value ? null : selectedBreakpoint(status.value))
watch(activeBreakpoint, (active) => {
  if (active?.line === selectedBreakpoint(status.value)?.line) breakpointSelection.value = null
})
const pendingZ = shallowRef<CommandBody | null>(null)
onBeforeUnmount(onResult((command, result) => {
  if (pendingZ.value === command) pendingZ.value = null
  const selected = breakpointSelection.value
  if (command.type === 'run' && command.breakpoint && result.outcome !== 'accepted' &&
    command.breakpoint.line === selected?.line && command.breakpoint.text === selected.text)
    breakpointSelection.value = null
}))
const confirmation = ref<{ context: number; unit: LengthUnit; airZ: number; breakpoint: BreakpointSelection | null } | null>(null)
const checkChanged = computed(() => confirmation.value !== null &&
  (checkedPreview.value?.context !== confirmation.value.context || panel.unit !== confirmation.value.unit ||
    airZ.value !== confirmation.value.airZ || breakpoint.value !== confirmation.value.breakpoint))
const touchOff = computed(() => panel.tab === 'probe')
const paused = computed(() => status.value?.wait.kind === 'paused')
const pausedZ = computed(() => paused.value && status.value?.mode === 'auto' ? status.value.paused_z : undefined)
const showZ = computed(() => panel.tab === 'program' && pausedZ.value !== undefined)
const zStep = computed(() => panel.unit === 'inch' ? 1.27 : 1)
const length = (mm: number): string => `${formatLength(mm, panel.unit)} ${panel.unit === 'inch' ? 'in' : 'mm'}`
const zBlocked = computed(() => {
  const reason = pausedZ.value?.reason ?? ''
  return linkBlock.value ?? (pendingZ.value ? 'Moving Z' : reason === '' ? null : reason)
})
const up = computed(() => Math.min(zStep.value, (pausedZ.value?.max_lift ?? 0) - (pausedZ.value?.offset ?? 0)))
const down = computed(() => Math.min(zStep.value, pausedZ.value?.offset ?? 0))
watch([live, computed(() => panel.hello)], () => { pendingZ.value = null })
function jogPausedZ(distance: number): void {
  if (zBlocked.value !== null || distance === 0) return
  const command = { type: 'jog_paused_z', distance } as const
  pendingZ.value = command
  send(command)
}
const toolChange = computed(() => status.value?.wait.kind === 'tool_change')
const programRunning = computed(() => status.value?.interp !== 'idle' && status.value?.mode === 'auto')
const probeWithSpindle = computed(() => {
  const spindle = status.value?.spindle
  return touchProbeLoaded.value && spindle !== undefined &&
    (spindle.direction !== 'stopped' || Math.abs(spindle.actual_rpm ?? 0) >= 1)
    ? 'A touch probe is loaded' : null
})

/** Why a program cannot be started, or null. The machine checks again; this only explains. */
const startBlock = computed((): string | null => {
  const shared = machineBlock(panel, true)
  if (shared !== null) return shared
  if (homingRequired.value && !allHomed.value) return 'Axes are not homed'
  if (!status.value?.program.file) return 'No program loaded'
  return probeWithSpindle.value
})

const runBlock = computed(() => startBlock.value ?? interpreterBlock(status.value) ??
  (airOwned.value ? 'Clearing the temporary air-cut offset' : null) ??
  (activeBreakpoint.value ? 'Reopening the original program' : null) ??
  ((panel.preview || airZ.value || breakpoint.value) && check.value === null ? 'Checking program' : null) ??
  (breakpoint.value && checkedPreview.value?.state !== 'ready' ? 'A breakpoint needs a complete program check' : null))

const pauseBlock = computed((): string | null => {
  const s = status.value
  if (linkBlock.value !== null || s === null) {
    return linkBlock.value ?? connectionBlock('connecting')
  }
  if (s.interp === 'idle') {
    return 'Nothing is running'
  }
  return toolChange.value ? 'Waiting for the tool change' : null
})

const stepBlock = computed((): string | null => {
  if (startBlock.value !== null) {
    return startBlock.value
  }
  if (airZ.value || airOwned.value) return 'Turn off air cut before using single block'
  if (pausedZ.value?.displaced) return 'Use Resume to return Z before continuing'
  if (activeBreakpoint.value) return 'Use Resume to continue the breakpoint run'
  return toolChange.value ? 'Waiting for the tool change' : null
})

const primary = computed(() => {
  if (paused.value) return {
    label: pausedZ.value?.displaced ? 'Return Z & resume' : 'Resume',
    icon: 'play' as const, type: 'resume' as const,
    blocked: linkBlock.value ?? probeWithSpindle.value ?? (pendingZ.value || pausedZ.value?.displaced ? zBlocked.value : null),
  }
  if (programRunning.value || status.value?.mode === 'mdi' && status.value.interp !== 'idle')
    return { label: 'Feed hold', icon: 'pause' as const, type: 'pause' as const, blocked: pauseBlock.value }
  const label = airZ.value ? 'Start air cut' : breakpoint.value ? `Run to line ${String(breakpoint.value.line)}` : 'Cycle start'
  return { label, icon: 'play' as const, type: 'run' as const, blocked: runBlock.value }
})
function primaryAction(): void {
  const { type, blocked } = primary.value
  if (blocked !== null) return
  if (type !== 'run') {
    const command = { type } as const
    if (type === 'resume' && pausedZ.value?.displaced) pendingZ.value = command
    send(command)
    return
  }
  if (confirmation.value !== null) return
  const preview = checkedPreview.value
  if (preview === null || check.value === null) {
    send({ type: 'run' })
    return
  }
  if (airZ.value || check.value.errors.length || check.value.warnings.length) {
    confirmation.value = { context: preview.context, unit: panel.unit, airZ: airZ.value, breakpoint: breakpoint.value }
  } else {
    send({ type: 'run', preview_context: preview.context, display_units: panel.unit,
      ...(breakpoint.value ? { breakpoint: { line: breakpoint.value.line, text: breakpoint.value.text } } : {}) })
  }
}
watch(breakpointSelection, (selection) => {
  if (selection && selection === breakpoint.value && primary.value.type === 'run') primaryAction()
})

function confirmRun(): void {
  const reviewed = confirmation.value
  if (reviewed === null || checkChanged.value || runBlock.value !== null ||
    check.value === null || check.value.errors.length > 0) return
  confirmation.value = null
  send({ type: 'run', preview_context: reviewed.context, display_units: reviewed.unit, confirmed: true,
    ...(reviewed.airZ ? { air_z: reviewed.airZ } : {}),
    ...(reviewed.breakpoint ? { breakpoint: { line: reviewed.breakpoint.line, text: reviewed.breakpoint.text } } : {}) })
}

function closeConfirmation(): void {
  if (confirmation.value?.breakpoint) breakpointSelection.value = null
  confirmation.value = null
}

function option(which: 'optional_stop' | 'block_delete'): void {
  send({ type: 'set_program_option', option: which, on: !(status.value?.program[which] ?? false) })
}
</script>

<template>
  <footer class="runbar" :class="{ 'touch-off': touchOff }">
    <OverrideControl v-if="!touchOff" label="Feed override" which="feed" :value="status?.motion.feed_override ?? null" :min="0" :max="panel.hello?.max_feed_override ?? 1" :disabled="!live" />
    <div v-if="showZ" v-tooltip="zBlocked" class="paused-z" role="group" aria-label="Paused Z lift">
      <div class="paused-z-reading">
        <span>Z lift</span>
        <strong class="mono">{{ length(pausedZ?.offset ?? 0) }}</strong>
      </div>
      <button
        v-tooltip="zBlocked ?? (down > 0 ? `Lower Z by ${length(down)}` : 'At the paused Z position')" class="btn" type="button"
        aria-label="Lower paused Z"
        :disabled="zBlocked !== null || down <= 0" @click="jogPausedZ(-down)"
      >
        −Z
      </button>
      <button
        v-tooltip="zBlocked ?? (up > 0 ? `Lift Z by ${length(up)}` : 'At the Z lift limit')" class="btn" type="button"
        aria-label="Lift paused Z"
        :disabled="zBlocked !== null || up <= 0" @click="jogPausedZ(up)"
      >
        +Z
      </button>
    </div>
    <OverrideControl v-else-if="!touchOff" label="Spindle override" which="spindle" :value="status?.spindle.override ?? null" :min="panel.hello?.min_spindle_override ?? 1" :max="panel.hello?.max_spindle_override ?? 1" :disabled="!live" />
    <div class="command-actions">
      <details v-if="!touchOff" v-popover class="more">
        <summary class="more-button" aria-label="More execution controls">···</summary>
        <div class="popover">
          <h3>Execution controls</h3>
          <OverrideControl label="Rapid override" which="rapid" :value="status?.motion.rapid_override ?? null" :min="0" :max="1" :disabled="!live" />
          <div class="actions">
            <button v-tooltip="stepBlock ?? 'Execute one block, then pause'" class="btn" type="button" :disabled="stepBlock !== null" @click="send({ type: 'step' })"><AppIcon name="step" />Single block</button>
            <button class="btn" type="button" :class="{ on: status?.program.optional_stop }" :disabled="!live" @click="option('optional_stop')">Optional stop M1</button>
            <button class="btn" type="button" :class="{ on: status?.program.block_delete }" :disabled="!live || airOwned || !!activeBreakpoint" @click="option('block_delete')">Block delete /</button>
          </div>
        </div>
      </details>
      <button class="btn stop-button" type="button" aria-label="Stop program, MDI, jog and spindle" @click="closeConfirmation(); breakpointSelection = null; send({ type: 'abort' })"><AppIcon name="stop" />Stop</button>
      <button v-if="!touchOff || primary.type === 'pause'" v-tooltip="primary.blocked ?? primary.label" class="btn primary main-action" type="button" :disabled="primary.blocked !== null || (primary.type === 'run' && confirmation !== null)" @click="primaryAction"><AppIcon :name="primary.icon" />{{ primary.label }}</button>
    </div>
    <ProgramCheckDialog
      v-if="confirmation" :check="check" :unit="panel.unit"
      :max-rpm="panel.hello?.max_spindle_rpm" :changed="checkChanged" :air-z="confirmation.airZ"
      :breakpoint-line="confirmation.breakpoint?.line ?? null" can-confirm
      @close="closeConfirmation" @confirm="confirmRun"
    />
  </footer>
</template>

<style scoped>
.runbar { display: grid; grid-template-columns: minmax(140px, 1fr) minmax(140px, 1fr) auto; gap: 18px; align-items: center; flex: none; padding: 7px 24px; border-top: 1px solid var(--line); background: var(--panel); }
.runbar.touch-off { grid-template-columns: 1fr; justify-items: end; }
.command-actions { display: flex; align-items: center; gap: 8px; }.command-actions > .btn { height: 48px; border-radius: 9px; font-size: 13px; }
.paused-z { display: flex; align-items: center; gap: 6px; justify-content: flex-end; }
.paused-z-reading { display: flex; flex-direction: column; min-width: 0; margin-right: 6px; font-size: 12px; color: var(--text-dim); }
.paused-z-reading strong { color: var(--text); white-space: nowrap; }
.paused-z .btn { min-width: 47px; height: 48px; padding: 0 8px; border-radius: 9px; }
.stop-button { color: var(--stop); min-width: 88px; }.main-action { min-width: 156px; }
.more { position: relative; }.more-button { display: flex; align-items: center; justify-content: center; width: 36px; height: 48px; font-size: 24px; color: var(--text-dim); }
.popover { bottom: calc(100% + 16px); right: -100px; }
</style>
