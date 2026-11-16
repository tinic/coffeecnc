<script setup lang="ts">
import { vTooltip } from '../tooltip'
// Always visible, always in the same place: the controls of a running program, the overrides and
// what the machine reports about feed, spindle and coolant.
import { computed } from 'vue'

import { allHomed, homingActive, homingRequired, linkBlock, live, panel, reported, send } from '../machine'

import AppIcon from './AppIcon.vue'
import OverrideControl from './OverrideControl.vue'

const status = reported
const paused = computed(() => status.value?.wait.kind === 'paused')
const toolChange = computed(() => status.value?.wait.kind === 'tool_change')
const programRunning = computed(() => status.value?.interp !== 'idle' && status.value?.mode === 'auto')

/** Why a program cannot be started, or null. The machine checks again; this only explains. */
const startBlock = computed((): string | null => {
  const s = status.value
  if (linkBlock.value !== null || s === null) {
    return linkBlock.value ?? 'No status from the machine yet'
  }
  if (s.machine !== 'on') {
    return s.machine === 'estop' ? 'Machine is in E-stop' : 'Machine is off'
  }
  if (homingActive.value) {
    return 'Homing is in progress'
  }
  if (homingRequired.value && !allHomed.value) {
    return 'Axes are not homed'
  }
  if (s.program.file === null) {
    return 'No program loaded'
  }
  if (s.interp !== 'idle') {
    return s.mode === 'mdi' ? 'An MDI command is running' : null
  }
  return null
})

const runBlock = computed((): string | null => {
  if (startBlock.value !== null) {
    return startBlock.value
  }
  if (paused.value) {
    return 'Program is paused: resume it, or stop it first'
  }
  return programRunning.value ? 'Program is running' : null
})

const pauseBlock = computed((): string | null => {
  const s = status.value
  if (linkBlock.value !== null || s === null) {
    return linkBlock.value ?? 'No status from the machine yet'
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
  return toolChange.value ? 'Waiting for the tool change' : null
})

const primary = computed(() => {
  if (paused.value) return { label: 'Resume', icon: 'play' as const, type: 'resume' as const, blocked: linkBlock.value }
  if (programRunning.value || status.value?.mode === 'mdi' && status.value.interp !== 'idle')
    return { label: 'Feed hold', icon: 'pause' as const, type: 'pause' as const, blocked: pauseBlock.value }
  return { label: 'Cycle start', icon: 'play' as const, type: 'run' as const, blocked: runBlock.value }
})
function option(which: 'optional_stop' | 'block_delete'): void {
  send({ type: 'set_program_option', option: which, on: !(status.value?.program[which] ?? false) })
}
</script>

<template>
  <footer class="runbar">
    <OverrideControl label="Feed override" which="feed" :value="status?.motion.feed_override ?? null" :min="0" :max="panel.hello?.max_feed_override ?? 1" :disabled="!live" />
    <OverrideControl label="Spindle override" which="spindle" :value="status?.spindle.override ?? null" :min="panel.hello?.min_spindle_override ?? 1" :max="panel.hello?.max_spindle_override ?? 1" :disabled="!live" />
    <div class="command-actions">
      <details class="more">
        <summary class="more-button" aria-label="More execution controls">···</summary>
        <div class="popover">
          <h3>Execution controls</h3>
          <OverrideControl label="Rapid override" which="rapid" :value="status?.motion.rapid_override ?? null" :min="0" :max="1" :disabled="!live" />
          <div class="actions">
            <button v-tooltip="stepBlock ?? 'Execute one block, then pause'" class="btn" type="button" :disabled="stepBlock !== null" @click="send({ type: 'step' })"><AppIcon name="step" />Single block</button>
            <button class="btn" type="button" :class="{ on: status?.program.optional_stop }" :disabled="!live" @click="option('optional_stop')">Optional stop M1</button>
            <button class="btn" type="button" :class="{ on: status?.program.block_delete }" :disabled="!live" @click="option('block_delete')">Block delete /</button>
          </div>
        </div>
      </details>
      <button class="btn stop-button" type="button" aria-label="Stop program, MDI, jog and spindle" @click="send({ type: 'abort' })"><AppIcon name="stop" />Stop</button>
      <button v-tooltip="primary.blocked ?? primary.label" class="btn primary main-action" type="button" :disabled="primary.blocked !== null" @click="send({ type: primary.type })"><AppIcon :name="primary.icon" />{{ primary.label }}</button>
    </div>
  </footer>
</template>

<style scoped>
.runbar { display: grid; grid-template-columns: minmax(140px, 1fr) minmax(140px, 1fr) auto; gap: 18px; align-items: center; flex: none; padding: 7px 24px; border-top: 1px solid var(--line); background: var(--panel); }
.command-actions { display: flex; align-items: center; gap: 8px; }.command-actions > .btn { height: 48px; border-radius: 9px; font-size: 13px; }
.stop-button { color: var(--stop); min-width: 88px; }.main-action { min-width: 156px; }
.more { position: relative; }.more-button { display: flex; align-items: center; justify-content: center; width: 36px; height: 48px; font-size: 24px; color: var(--text-dim); }
.popover { bottom: calc(100% + 16px); right: -100px; }
</style>
