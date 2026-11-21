<script setup lang="ts">
import { vTooltip } from '../tooltip'
import { vPopover } from '../popover'
// Keep Stop available in every workspace. Touch-off needs no program setup controls.
import { computed, ref, watch } from 'vue'

import { allHomed, homingRequired, linkBlock, live, panel, reported, send } from '../machine'
import { connectionBlock, interpreterBlock, machineBlock } from '../machine-block'
import { currentProgramPreview, describeProgramCheck } from '../program-check'
import { breakpointSelection, selectedBreakpoint } from '../program-breakpoint'
import type { LengthUnit } from '../protocol'
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
  if (active?.line === selectedBreakpoint(status.value)) breakpointSelection.value = null
})
const confirmation = ref<{ context: number; unit: LengthUnit; airZ: number; breakpoint: number | null } | null>(null)
const checkChanged = computed(() => confirmation.value !== null &&
  (checkedPreview.value?.context !== confirmation.value.context || panel.unit !== confirmation.value.unit ||
    airZ.value !== confirmation.value.airZ || breakpoint.value !== confirmation.value.breakpoint))
const touchOff = computed(() => panel.tab === 'probe')
const paused = computed(() => status.value?.wait.kind === 'paused')
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
  if (activeBreakpoint.value) return 'Use Resume to continue the breakpoint run'
  return toolChange.value ? 'Waiting for the tool change' : null
})

const primary = computed(() => {
  if (paused.value) return { label: 'Resume', icon: 'play' as const, type: 'resume' as const, blocked: linkBlock.value ?? probeWithSpindle.value }
  if (programRunning.value || status.value?.mode === 'mdi' && status.value.interp !== 'idle')
    return { label: 'Feed hold', icon: 'pause' as const, type: 'pause' as const, blocked: pauseBlock.value }
  const label = airZ.value ? 'Start air cut' : breakpoint.value ? `Run to line ${String(breakpoint.value)}` : 'Cycle start'
  return { label, icon: 'play' as const, type: 'run' as const, blocked: runBlock.value }
})
function primaryAction(): void {
  const { type, blocked } = primary.value
  if (blocked !== null) return
  if (type !== 'run') {
    send({ type })
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
      ...(breakpoint.value ? { breakpoint: breakpoint.value } : {}) })
  }
}

function confirmRun(): void {
  const reviewed = confirmation.value
  if (reviewed === null || checkChanged.value || runBlock.value !== null ||
    check.value === null || check.value.errors.length > 0) return
  confirmation.value = null
  send({ type: 'run', preview_context: reviewed.context, display_units: reviewed.unit, confirmed: true,
    ...(reviewed.airZ ? { air_z: reviewed.airZ } : {}),
    ...(reviewed.breakpoint ? { breakpoint: reviewed.breakpoint } : {}) })
}

function option(which: 'optional_stop' | 'block_delete'): void {
  send({ type: 'set_program_option', option: which, on: !(status.value?.program[which] ?? false) })
}
</script>

<template>
  <footer class="runbar" :class="{ 'touch-off': touchOff }">
    <OverrideControl v-if="!touchOff" label="Feed override" which="feed" :value="status?.motion.feed_override ?? null" :min="0" :max="panel.hello?.max_feed_override ?? 1" :disabled="!live" />
    <OverrideControl v-if="!touchOff" label="Spindle override" which="spindle" :value="status?.spindle.override ?? null" :min="panel.hello?.min_spindle_override ?? 1" :max="panel.hello?.max_spindle_override ?? 1" :disabled="!live" />
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
      <button class="btn stop-button" type="button" aria-label="Stop program, MDI, jog and spindle" @click="confirmation = null; send({ type: 'abort' })"><AppIcon name="stop" />Stop</button>
      <button v-if="!touchOff || primary.type === 'pause'" v-tooltip="primary.blocked ?? primary.label" class="btn primary main-action" type="button" :disabled="primary.blocked !== null || (primary.type === 'run' && confirmation !== null)" @click="primaryAction"><AppIcon :name="primary.icon" />{{ primary.label }}</button>
    </div>
    <ProgramCheckDialog
      v-if="confirmation" :check="check" :unit="panel.unit"
      :max-rpm="panel.hello?.max_spindle_rpm" :changed="checkChanged" :air-z="confirmation.airZ"
      :breakpoint-line="confirmation.breakpoint" can-confirm
      @close="confirmation = null" @confirm="confirmRun"
    />
  </footer>
</template>

<style scoped>
.runbar { display: grid; grid-template-columns: minmax(140px, 1fr) minmax(140px, 1fr) auto; gap: 18px; align-items: center; flex: none; padding: 7px 24px; border-top: 1px solid var(--line); background: var(--panel); }
.runbar.touch-off { grid-template-columns: 1fr; justify-items: end; }
.command-actions { display: flex; align-items: center; gap: 8px; }.command-actions > .btn { height: 48px; border-radius: 9px; font-size: 13px; }
.stop-button { color: var(--stop); min-width: 88px; }.main-action { min-width: 156px; }
.more { position: relative; }.more-button { display: flex; align-items: center; justify-content: center; width: 36px; height: 48px; font-size: 24px; color: var(--text-dim); }
.popover { bottom: calc(100% + 16px); right: -100px; }
</style>
