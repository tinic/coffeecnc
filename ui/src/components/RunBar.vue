<script setup lang="ts">
// Always visible, always in the same place: the controls of a running program, the overrides and
// what the machine reports about feed, spindle and coolant.
import { computed } from 'vue'

import { allHomed, homingActive, linkBlock, live, panel, reported, send } from '../machine'
import { feedUnit, formatProgrammedFeed, formatVelocity, velocityUnit } from '../units'
import AppIcon from './AppIcon.vue'
import OverrideControl from './OverrideControl.vue'

const status = reported
const unit = computed(() => panel.unit)
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
  if (!allHomed.value) {
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

/** The one line under the buttons: the most useful reason something is unavailable. */
const hint = computed((): string => {
  if (linkBlock.value !== null) {
    return `${linkBlock.value}. Stop and E-stop are still sent.`
  }
  if (programRunning.value || paused.value) {
    return paused.value ? 'Paused. Resume continues, Step runs one block.' : ''
  }
  return startBlock.value === null ? '' : `Cycle start unavailable: ${startBlock.value}`
})

const spindleDirection = computed(() => status.value?.spindle.direction ?? 'stopped')
const spindleRpm = computed(() => {
  const s = status.value
  return s === null ? 0 : Math.round(s.spindle.commanded_rpm * s.spindle.override)
})
</script>

<template>
  <footer class="runbar">
    <div class="run">
      <div class="buttons">
        <button
          class="btn go big"
          type="button"
          :disabled="runBlock !== null"
          :title="runBlock ?? 'Run the loaded program from its first line'"
          @click="send({ type: 'run' })"
        >
          <AppIcon name="play" />Cycle start
        </button>
        <button
          v-if="paused"
          class="btn on big"
          type="button"
          :disabled="linkBlock !== null"
          :title="linkBlock ?? 'Continue the program'"
          @click="send({ type: 'resume' })"
        >
          <AppIcon name="play" />Resume
        </button>
        <button
          v-else
          class="btn big"
          type="button"
          :disabled="pauseBlock !== null"
          :title="pauseBlock ?? 'Feed hold: motion stops, the spindle keeps running'"
          @click="send({ type: 'pause' })"
        >
          <AppIcon name="pause" />Pause
        </button>
        <button
          class="btn big"
          type="button"
          :disabled="stepBlock !== null"
          :title="stepBlock ?? 'Run one block, then pause'"
          @click="send({ type: 'step' })"
        >
          <AppIcon name="step" />Step
        </button>
        <button
          class="btn danger big"
          type="button"
          title="Stops the program or MDI command, jogging, and the spindle. Not an E-stop."
          @click="send({ type: 'abort' })"
        >
          <AppIcon name="stop" />Stop
        </button>
      </div>
      <span class="hint" :class="{ blocked: hint !== '' && !paused }">{{ hint }}</span>
    </div>

    <div class="overrides">
      <OverrideControl
        label="Feed"
        which="feed"
        :value="status?.motion.feed_override ?? null"
        :min="0"
        :max="panel.hello?.max_feed_override ?? 1"
        :disabled="!live"
      />
      <OverrideControl
        label="Rapid"
        which="rapid"
        :value="status?.motion.rapid_override ?? null"
        :min="0"
        :max="1"
        :disabled="!live"
      />
      <OverrideControl
        label="Spindle"
        which="spindle"
        :value="status?.spindle.override ?? null"
        :min="panel.hello?.min_spindle_override ?? 1"
        :max="panel.hello?.max_spindle_override ?? 1"
        :disabled="!live"
      />
    </div>

    <div class="readouts" :class="{ distrust: !live }">
      <div class="readout">
        <span class="label">Feed</span>
        <span class="big mono">
          {{ status ? formatVelocity(status.motion.velocity, unit) : '—' }}
          <span class="unit">{{ velocityUnit(unit) }}</span>
        </span>
        <span class="small" title="The F word in effect">
          F
          <span class="mono">{{
            status ? formatProgrammedFeed(status.motion.programmed_feed, status.motion.feed_mode, unit) : '—'
          }}</span>
          {{ status ? feedUnit(status.motion.feed_mode, unit) : '' }}
        </span>
      </div>
      <div class="readout">
        <span class="label">Spindle</span>
        <span class="big mono" :class="{ idle: spindleDirection === 'stopped' }">
          <AppIcon v-if="spindleDirection !== 'stopped'" :name="spindleDirection" />
          {{ spindleDirection === 'stopped' ? 'Stopped' : spindleRpm }}
          <span v-if="spindleDirection !== 'stopped'" class="unit">rpm</span>
        </span>
        <span class="small" title="LinuxCNC reports the commanded speed; the actual speed is not measured here.">
          <template v-if="spindleDirection !== 'stopped'">{{ spindleDirection }}, commanded</template>
          <template v-else>S <span class="mono">{{ status ? Math.round(status.spindle.commanded_rpm) : '—' }}</span> rpm set</template>
        </span>
      </div>
      <div class="readout coolant">
        <span class="label">Coolant</span>
        <span class="chip" :class="{ go: status?.coolant.flood }">Flood {{ status?.coolant.flood ? 'on' : 'off' }}</span>
        <span class="chip" :class="{ go: status?.coolant.mist }">Mist {{ status?.coolant.mist ? 'on' : 'off' }}</span>
      </div>
    </div>
  </footer>
</template>

<style scoped>
.runbar {
  display: flex;
  align-items: stretch;
  gap: 12px;
  height: 100px;
  padding: 8px 12px;
  border-top: 1px solid var(--line);
  background: var(--panel);
}

.run {
  display: flex;
  flex: none;
  flex-direction: column;
  gap: 4px;
}

.buttons {
  display: flex;
  gap: 8px;
}

.btn.big {
  flex-direction: column;
  gap: 2px;
  width: 78px;
  height: 62px;
  padding: 0;
  font-size: 13px;
}

.run .hint {
  height: 16px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.overrides {
  display: flex;
  flex: 1 1 0;
  gap: 12px;
  min-width: 0;
  padding: 0 12px;
  border-right: 1px solid var(--line);
  border-left: 1px solid var(--line);
}

.readouts {
  display: flex;
  flex: none;
  gap: 12px;
}

.readout {
  display: flex;
  flex-direction: column;
  gap: 2px;
  width: 138px;
  white-space: nowrap;
}

.unit {
  align-self: flex-end;
  padding-bottom: 5px;
  font-family: var(--sans);
  font-size: 12px;
  font-weight: 400;
  color: var(--text-dim);
}

.readout .big {
  display: flex;
  align-items: center;
  gap: 6px;
  height: 34px;
  font-size: 26px;
  font-weight: 600;
}

.readout .big.idle {
  font-family: var(--sans);
  font-size: 18px;
  color: var(--text-dim);
}

.small {
  font-size: 12px;
  color: var(--text-dim);
}

.coolant {
  width: 84px;
  gap: 6px;
}

.coolant .chip {
  justify-content: center;
  color: var(--text-dim);
}

.coolant .chip.go {
  color: var(--go);
}

.distrust .big,
.distrust .chip {
  color: var(--text-faint);
  text-decoration: line-through;
  text-decoration-thickness: 1px;
}
</style>
