<script setup lang="ts">
import { vTooltip } from '../tooltip'
import { vPopover } from '../popover'
// Keep Stop available in every workspace. Touch-off needs no program setup controls.
import { computed, onBeforeUnmount, ref, shallowRef, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { translateMessage } from '../i18n/messages'

import { allHomed, homingRequired, linkBlock, live, onResult, panel, reported, send } from '../machine'
import { connectionBlock, interpreterBlock, machineBlock } from '../machine-block'
import { currentProgramPreview, describeProgramCheck } from '../program-check'
import { breakpointSelection, selectedBreakpoint, type BreakpointSelection } from '../program-breakpoint'
import type { CommandBody, LengthUnit } from '../protocol'
import { formatLength } from '../units'
import { touchProbeLoaded } from '../tool-models'

import AppIcon from './AppIcon.vue'
import ModalDialog from './ModalDialog.vue'
import OverrideControl from './OverrideControl.vue'
import ProgramCheckDialog from './ProgramCheckDialog.vue'

const { t } = useI18n()
const status = reported
const checkedPreview = computed(() => currentProgramPreview(panel.preview ?? null, status.value))
const check = computed(() => describeProgramCheck(checkedPreview.value, panel.unit))
const activeBreakpoint = computed(() => status.value?.program.breakpoint)
const breakpoint = computed(() => activeBreakpoint.value ? null : selectedBreakpoint(status.value))
watch(activeBreakpoint, (active) => {
  if (active?.line === selectedBreakpoint(status.value)?.line) breakpointSelection.value = null
})
const pendingZ = shallowRef<CommandBody | null>(null)
const spindleConfirm = shallowRef<{ distance: number; pause: string } | null>(null)
onBeforeUnmount(onResult((command, result) => {
  if (pendingZ.value === command) pendingZ.value = null
  const selected = breakpointSelection.value
  if (command.type === 'run' && command.breakpoint && result.outcome !== 'accepted' &&
    command.breakpoint.line === selected?.line && command.breakpoint.text === selected.text)
    breakpointSelection.value = null
}))
const confirmation = ref<{ context: number; unit: LengthUnit; breakpoint: BreakpointSelection | null } | null>(null)
const checkChanged = computed(() => confirmation.value !== null &&
  (checkedPreview.value?.context !== confirmation.value.context || panel.unit !== confirmation.value.unit ||
    breakpoint.value !== confirmation.value.breakpoint))
const touchOff = computed(() => panel.tab === 'probe')
const paused = computed(() => status.value?.wait.kind === 'paused')
const pausedZ = computed(() => paused.value && status.value?.mode === 'auto' ? status.value.paused_z : undefined)
const showZ = computed(() => panel.tab === 'program' && pausedZ.value !== undefined)
const zStep = computed(() => panel.unit === 'inch' ? 1.27 : 1)
const length = (mm: number): string => `${formatLength(mm, panel.unit)} ${panel.unit === 'inch' ? 'in' : 'mm'}`
const zBlocked = computed(() => {
  const reason = pausedZ.value?.reason ?? ''
  return linkBlock.value ?? (pendingZ.value ? t('status.runBar.pausedZBusy') : reason === '' ? null : translateMessage(reason))
})
const up = computed(() => Math.min(zStep.value, (pausedZ.value?.max_lift ?? 0) - (pausedZ.value?.offset ?? 0)))
const down = computed(() => Math.min(zStep.value, pausedZ.value?.offset ?? 0))
const canStopSpindle = computed(() => showZ.value && live.value && pendingZ.value === null && pausedZ.value?.spindle === 'can_stop')
const spindlePause = computed(() => {
  const s = status.value
  if (!s || !canStopSpindle.value) return ''
  return JSON.stringify([s.program.file, s.program.revision, s.program.current_line,
    s.tool.in_spindle, s.spindle.direction, s.spindle.commanded_rpm])
})
watch(spindlePause, () => { spindleConfirm.value = null })
watch([live, computed(() => panel.hello)], () => { pendingZ.value = null; spindleConfirm.value = null })
function sendZ(distance: number, stopSpindle = false): void {
  const command = { type: 'jog_paused_z', distance, ...(stopSpindle ? { stop_spindle: true as const } : {}) } as const
  pendingZ.value = command
  send(command)
}
function jogPausedZ(distance: number): void {
  if (distance > 0 && spindlePause.value) {
    spindleConfirm.value = { distance, pause: spindlePause.value }
  } else if (zBlocked.value === null && distance !== 0) {
    sendZ(distance)
  }
}
function confirmSpindleStop(): void {
  const request = spindleConfirm.value
  spindleConfirm.value = null
  if (request?.pause === spindlePause.value) sendZ(request.distance, true)
}
const toolChange = computed(() => status.value?.wait.kind === 'tool_change')
const programRunning = computed(() => status.value?.interp !== 'idle' && status.value?.mode === 'auto')
const probeWithSpindle = computed(() => {
  const spindle = status.value?.spindle
  return touchProbeLoaded.value && spindle !== undefined &&
    (spindle.direction !== 'stopped' || Math.abs(spindle.actual_rpm ?? 0) >= 1)
    ? t('status.runBar.probeLoaded') : null
})

/** Why a program cannot be started, or null. The machine checks again; this only explains. */
const startBlock = computed((): string | null => {
  const shared = machineBlock(panel, true)
  if (shared !== null) return shared
  if (homingRequired.value && !allHomed.value) return t('status.machineBlock.axesNotHomed')
  if (!status.value?.program.file) return t('status.runBar.noProgram')
  return probeWithSpindle.value
})

const runBlock = computed(() => startBlock.value ?? interpreterBlock(status.value) ??
  (activeBreakpoint.value ? t('status.runBar.reopening') : null) ??
  ((panel.preview || breakpoint.value) && check.value === null ? t('status.runBar.checking') : null) ??
  (breakpoint.value && checkedPreview.value?.state !== 'ready' ? t('status.runBar.breakpointCheck') : null))

const pauseBlock = computed((): string | null => {
  const s = status.value
  if (linkBlock.value !== null || s === null) {
    return linkBlock.value ?? connectionBlock('connecting')
  }
  if (s.interp === 'idle') {
    return t('status.runBar.nothingRunning')
  }
  return toolChange.value ? t('status.runBar.waitingToolChange') : null
})

const stepBlock = computed((): string | null => {
  if (spindleConfirm.value !== null) return t('status.runBar.dismissSpindleConfirm')
  if (startBlock.value !== null) {
    return startBlock.value
  }
  if (pausedZ.value?.spindle === 'recover') return t('status.runBar.recoverSpindle')
  if (pausedZ.value?.spindle === 'restore') return t('status.runBar.restoreSpindle')
  if (pausedZ.value?.displaced) return t('status.runBar.returnZFirst')
  if (activeBreakpoint.value) return t('status.runBar.continueBreakpoint')
  return toolChange.value ? t('status.runBar.waitingToolChange') : null
})

const primary = computed(() => {
  if (paused.value) return {
    label: pendingZ.value?.type === 'resume'
      ? pausedZ.value?.spindle === 'restore' && !pausedZ.value.displaced ? t('status.runBar.restoringSpindle') : t('status.runBar.returningZ')
      : pausedZ.value?.displaced ? t('status.runBar.returnZResume')
      : pausedZ.value?.spindle === 'restore' ? t('status.runBar.restoreSpindleResume') : t('status.runBar.resume'),
    icon: 'play' as const, type: 'resume' as const,
    blocked: linkBlock.value ?? probeWithSpindle.value ??
      (pendingZ.value || pausedZ.value?.displaced || pausedZ.value?.spindle === 'recover' ||
        pausedZ.value?.spindle === 'restore' ? zBlocked.value : null),
  }
  if (programRunning.value || status.value?.mode === 'mdi' && status.value.interp !== 'idle')
    return { label: t('status.runBar.feedHold'), icon: 'pause' as const, type: 'pause' as const, blocked: pauseBlock.value }
  const label = breakpoint.value ? t('status.runBar.runToLine', { line: String(breakpoint.value.line) }) : t('status.runBar.cycleStart')
  return { label, icon: 'play' as const, type: 'run' as const, blocked: runBlock.value }
})
function primaryAction(): void {
  const { type, blocked } = primary.value
  if (blocked !== null) return
  if (type !== 'run') {
    const command = { type } as const
    if (type === 'resume' && (pausedZ.value?.displaced || pausedZ.value?.spindle === 'restore'))
      pendingZ.value = command
    send(command)
    return
  }
  if (confirmation.value !== null) return
  const preview = checkedPreview.value
  if (preview === null || check.value === null) {
    send({ type: 'run' })
    return
  }
  if (check.value.errors.length || check.value.warnings.length) {
    confirmation.value = { context: preview.context, unit: panel.unit, breakpoint: breakpoint.value }
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
    <OverrideControl v-if="!touchOff" :label="t('status.runBar.feedOverride')" which="feed" :value="status?.motion.feed_override ?? null" :min="0" :max="panel.hello?.max_feed_override ?? 1" :disabled="!live" />
    <div v-if="showZ" v-tooltip="zBlocked" class="paused-z" role="group" :aria-label="t('status.runBar.pausedZLift')">
      <div class="paused-z-reading">
        <span>{{ t('status.runBar.zLift') }}</span>
        <strong class="mono">{{ length(pausedZ?.offset ?? 0) }}</strong>
      </div>
      <button
        v-tooltip="zBlocked ?? (down > 0 ? t('status.runBar.lowerBy', { length: length(down) }) : t('status.runBar.atPausedZ'))" class="btn" type="button"
        :aria-label="t('status.runBar.lowerPausedZ')"
        :disabled="zBlocked !== null || down <= 0" @click="jogPausedZ(-down)"
      >
        −Z
      </button>
      <button
        v-tooltip="canStopSpindle ? t('status.runBar.stopAndLift') : zBlocked ?? (up > 0 ? t('status.runBar.liftBy', { length: length(up) }) : t('status.runBar.atLiftLimit'))" class="btn" type="button"
        :aria-label="t('status.runBar.liftPausedZ')"
        :disabled="(!canStopSpindle && zBlocked !== null) || up <= 0" @click="jogPausedZ(up)"
      >
        +Z
      </button>
    </div>
    <OverrideControl v-else-if="!touchOff" :label="t('status.runBar.spindleOverride')" which="spindle" :value="status?.spindle.override ?? null" :min="panel.hello?.min_spindle_override ?? 1" :max="panel.hello?.max_spindle_override ?? 1" :disabled="!live" />
    <div class="command-actions">
      <details v-if="!touchOff" v-popover class="more">
        <summary class="more-button" :aria-label="t('status.runBar.moreControls')">···</summary>
        <div class="popover">
          <h3>{{ t('status.runBar.executionControls') }}</h3>
          <OverrideControl :label="t('status.runBar.rapidOverride')" which="rapid" :value="status?.motion.rapid_override ?? null" :min="0" :max="1" :disabled="!live" />
          <div class="actions">
            <button v-tooltip="stepBlock ?? t('status.runBar.singleBlockTip')" class="btn" type="button" :disabled="stepBlock !== null" @click="send({ type: 'step' })"><AppIcon name="step" />{{ t('status.runBar.singleBlock') }}</button>
            <button class="btn" type="button" :class="{ on: status?.program.optional_stop }" :disabled="!live" @click="option('optional_stop')">{{ t('status.runBar.optionalStop') }}</button>
            <button class="btn" type="button" :class="{ on: status?.program.block_delete }" :disabled="!live || !!activeBreakpoint" @click="option('block_delete')">{{ t('status.runBar.blockDelete') }}</button>
          </div>
        </div>
      </details>
      <button class="btn stop-button" type="button" :aria-label="t('status.runBar.stopTip')" @click="closeConfirmation(); spindleConfirm = null; breakpointSelection = null; send({ type: 'abort' })"><AppIcon name="stop" />{{ t('status.runBar.stop') }}</button>
      <button v-if="!touchOff || primary.type === 'pause'" v-tooltip="primary.blocked ?? primary.label" class="btn primary main-action" type="button" :disabled="primary.blocked !== null || spindleConfirm !== null || (primary.type === 'run' && confirmation !== null)" @click="primaryAction"><AppIcon :name="primary.icon" />{{ primary.label }}</button>
    </div>
    <ModalDialog v-if="spindleConfirm" :title="t('status.runBar.spindleConfirmTitle')" size="small" fresh-gesture @close="spindleConfirm = null">
      <p class="spindle-effect">{{ t('status.runBar.spindleConfirmEffect', { length: length(spindleConfirm.distance) }) }}</p>
      <template #footer>
        <button class="btn" type="button" @click="spindleConfirm = null">{{ t('common.cancel') }}</button>
        <button class="btn primary" type="button" @click="confirmSpindleStop">{{ t('status.runBar.stopSpindleLift') }}</button>
      </template>
    </ModalDialog>
    <ProgramCheckDialog
      v-if="confirmation" :check="check" :unit="panel.unit"
      :max-rpm="panel.hello?.max_spindle_rpm" :changed="checkChanged"
      :breakpoint-line="confirmation.breakpoint?.line ?? null" can-confirm
      @close="closeConfirmation" @confirm="confirmRun"
    />
  </footer>
</template>

<style scoped>
.runbar { display: grid; grid-template-columns: minmax(140px, 1fr) minmax(140px, 1fr) auto; gap: 18px; align-items: center; flex: none; padding: 7px 24px; border-top: 1px solid var(--line); background: var(--panel); }
.runbar.touch-off { grid-template-columns: 1fr; justify-items: end; }
.command-actions { display: flex; align-items: center; gap: 8px; }.command-actions > .btn { height: 48px; border-radius: 9px; font-size: 13px; }
.spindle-effect { margin: 0; line-height: 1.55; }
.paused-z { display: flex; align-items: center; gap: 6px; justify-content: flex-end; }
.paused-z-reading { display: flex; flex-direction: column; min-width: 0; margin-right: 6px; font-size: 12px; color: var(--text-dim); }
.paused-z-reading strong { color: var(--text); white-space: nowrap; }
.paused-z .btn { min-width: 47px; height: 48px; padding: 0 8px; border-radius: 9px; }
.stop-button { color: var(--stop); min-width: 88px; }
/* Keep Stop and the Z buttons stationary when the primary caption changes. */
.main-action { width: 212px; }
.more { position: relative; }.more-button { display: flex; align-items: center; justify-content: center; width: 36px; height: 48px; font-size: 24px; color: var(--text-dim); }
.popover { bottom: calc(100% + 16px); right: -100px; }
</style>
