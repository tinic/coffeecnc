<script setup lang="ts">
// The position readout. It shows what the machine reports and nothing else: unhomed axes and
// status that is not live look different from trusted numbers.
import { computed, ref } from 'vue'

import { startDialog } from '../dialogs'
import { axes, findTool, linkBlock, live, mdiBlock, panel, reported, send } from '../machine'
import type { AxisLetter, AxisStatus } from '../protocol'
import { baseName, formatLength, fromDisplay, wcsName } from '../units'
import AppIcon from './AppIcon.vue'
import NumberDialog from './NumberDialog.vue'

const status = reported
const unit = computed(() => panel.unit)
const wcs = computed(() => wcsName(status.value?.offsets.wcs ?? 1))

const touching = ref<AxisLetter | null>(startDialog === 'touchoff' ? 'x' : null)

function marker(axis: AxisStatus): string {
  if (axis.homing) {
    return 'Homing'
  }
  if (axis.on_hard_limit) {
    return 'On limit'
  }
  return axis.homed ? '' : 'Not homed'
}

function touchOff(value: number): void {
  if (touching.value !== null) {
    send({ type: 'touch_off', axis: touching.value, value: fromDisplay(value, unit.value) })
  }
  touching.value = null
}

const g92 = computed(() => status.value?.offsets.g92.some((v) => v !== 0) ?? false)
const rotation = computed(() => status.value?.offsets.rotation_xy ?? 0)
const toolOffset = computed(() => status.value?.offsets.tool[2] ?? 0)

const state = computed((): { title: string; detail: string; tone: '' | 'warn' | 'go' } => {
  const s = status.value
  if (s === null) {
    return { title: 'No status', detail: linkBlock.value ?? '', tone: 'warn' }
  }
  const file = s.program.file === null ? 'No program loaded' : baseName(s.program.file)
  const total = panel.program?.revision === s.program.revision ? panel.program.lines.length : 0
  const line = s.program.current_line
  const of = total > 0 ? ` of ${String(total)}` : ''
  if (s.wait.kind === 'tool_change') {
    const tool = findTool(s.wait.tool)
    return {
      title: s.wait.tool === 0 ? 'Waiting: remove the tool' : `Waiting for tool change: T${String(s.wait.tool)}`,
      detail: tool?.comment ?? '',
      tone: 'warn',
    }
  }
  if (s.wait.kind === 'paused') {
    const spindle = s.spindle.direction === 'stopped' ? 'Spindle stopped' : `Spindle running ${s.spindle.direction}`
    return {
      title: line > 0 ? `Paused at line ${String(line)}` : 'Paused',
      detail: `${spindle}. Resume or stop.`,
      tone: 'warn',
    }
  }
  if (s.wait.kind === 'dwell') {
    return { title: `Dwell: ${s.wait.seconds_left.toFixed(1)} s left`, detail: line > 0 ? `Line ${String(line)}${of}` : '', tone: 'go' }
  }
  if (s.interp !== 'idle') {
    return s.mode === 'mdi'
      ? { title: 'MDI command running', detail: '', tone: 'go' }
      : { title: `Running line ${String(line)}${of}`, detail: file, tone: 'go' }
  }
  return { title: 'Idle', detail: file, tone: '' }
})
</script>

<template>
  <section class="dro" :class="{ distrust: !live }">
    <div class="head">
      <div class="basis">
        <span class="label">Position</span>
        <span class="system">Work <span class="mono">{{ wcs }}</span></span>
      </div>
      <div class="segmented" title="Display unit. The machine and its programs are not changed.">
        <button class="btn" type="button" :class="{ on: unit === 'mm' }" @click="panel.unit = 'mm'">mm</button>
        <button class="btn" type="button" :class="{ on: unit === 'inch' }" @click="panel.unit = 'inch'">inch</button>
      </div>
      <button
        class="btn"
        type="button"
        :class="{ on: panel.droDetails }"
        title="Show machine coordinates and distance to go"
        @click="panel.droDetails = !panel.droDetails"
      >
        Mach · DTG
      </button>
    </div>

    <p v-if="!live" class="stale">
      <AppIcon name="warning" />
      <span v-if="panel.link === 'connecting'">No status yet</span>
      <span v-else-if="panel.link === 'down'">No connection to LinuxCNC. The position is not known.</span>
      <span v-else-if="panel.link === 'stale'">
        LinuxCNC has stopped updating. These are the last values it wrote, not the present position.
      </span>
      <span v-else>No updates. These are the last values received, not the present position.</span>
    </p>

    <div v-for="axis in axes" :key="axis.axis" class="axis" :class="{ unhomed: !axis.homed, homing: axis.homing }">
      <div class="letter">
        <span>{{ axis.axis.toUpperCase() }}</span>
        <span v-if="marker(axis)" class="marker">{{ marker(axis) }}</span>
      </div>
      <div class="numbers">
        <span class="work mono">{{ formatLength(axis.work, unit) }}</span>
        <span v-if="panel.droDetails" class="detail mono">
          <span class="tag">Mach</span>{{ formatLength(axis.machine, unit) }}
          <span class="tag gap">DTG</span>{{ formatLength(axis.dtg, unit) }}
        </span>
      </div>
      <button
        v-if="panel.tab === 'manual'"
        class="btn square touch"
        type="button"
        :disabled="mdiBlock !== null"
        :title="mdiBlock ?? `Touch off ${axis.axis.toUpperCase()} in ${wcs}`"
        @click="touching = axis.axis"
      >
        <AppIcon name="target" />
      </button>
    </div>
    <div v-if="axes.length === 0" class="axis empty">No axis status</div>

    <div class="facts">
      <span class="chip" :class="{ warn: toolOffset === 0 && (status?.tool.in_spindle ?? 0) !== 0 }">
        <template v-if="toolOffset !== 0">Tool length {{ formatLength(toolOffset, unit) }} applied</template>
        <template v-else>No tool length applied</template>
      </span>
      <span v-if="g92" class="chip warn" title="A G92 offset shifts every work system.">G92 offset active</span>
      <span v-if="rotation !== 0" class="chip warn">Rotated {{ rotation }}°</span>
      <span class="chip">Program in {{ status?.program_units ?? '—' }}</span>
    </div>

    <div class="state" :class="state.tone">
      <span class="label">Program state</span>
      <span class="title">{{ state.title }}</span>
      <span class="detail">{{ state.detail }}</span>
    </div>

    <div class="codes">
      <span class="label">Active codes</span>
      <span class="mono list">{{ status?.active_codes.join(' ') ?? '—' }}</span>
    </div>

    <NumberDialog
      v-if="touching !== null"
      :title="`Touch off ${touching.toUpperCase()} in ${wcs}`"
      :effect="`Changes the ${wcs} work offset so that ${touching.toUpperCase()} reads the value below at the present position. The machine does not move.`"
      :label="`${touching.toUpperCase()} is now at`"
      :unit="unit"
      initial="0"
      :action="`Set ${wcs} ${touching.toUpperCase()}`"
      @confirm="touchOff"
      @close="touching = null"
    />
  </section>
</template>

<style scoped>
.dro {
  display: flex;
  flex-direction: column;
  gap: 8px;
  width: 376px;
  min-height: 0;
  padding: 10px 12px;
  border-right: 1px solid var(--line);
  background: var(--panel);
  overflow: hidden;
}

.head {
  display: flex;
  align-items: center;
  gap: 8px;
}

.basis {
  display: flex;
  flex: 1;
  flex-direction: column;
  gap: 2px;
}

.system {
  font-size: 16px;
  font-weight: 600;
}

.head .btn {
  padding: 0 10px;
  font-size: 13px;
}

.stale {
  display: flex;
  align-items: center;
  gap: 8px;
  margin: 0;
  padding: 6px 10px;
  border: 1px solid var(--stop);
  border-radius: var(--radius);
  background: var(--stop-soft);
  font-size: 13px;
  font-weight: 600;
  color: var(--stop);
}

.axis {
  display: flex;
  align-items: center;
  gap: 8px;
  min-height: 80px;
  padding: 6px 8px 6px 12px;
  border: 1px solid var(--line);
  border-radius: var(--radius);
  background: var(--bg-sunken);
}

.axis.empty {
  justify-content: center;
  color: var(--text-faint);
}

.letter {
  display: flex;
  flex-direction: column;
  gap: 2px;
  width: 62px;
  font-size: 30px;
  font-weight: 700;
  line-height: 1;
  color: var(--text-dim);
}

.marker {
  font-size: 10px;
  font-weight: 700;
  letter-spacing: 0.05em;
  text-transform: uppercase;
  color: var(--warn);
}

.numbers {
  display: flex;
  flex: 1;
  flex-direction: column;
  align-items: flex-end;
  min-width: 0;
}

.work {
  font-size: 40px;
  font-weight: 600;
  line-height: 1.1;
  letter-spacing: -0.01em;
}

.detail {
  font-size: 13px;
  color: var(--text-dim);
}

.tag {
  margin-right: 6px;
  font-family: var(--sans);
  font-size: 10px;
  font-weight: 600;
  letter-spacing: 0.06em;
  text-transform: uppercase;
  color: var(--text-faint);
}

.tag.gap {
  margin-left: 10px;
}

/* An unhomed axis counts from wherever it was switched on: the number is not a machine position. */
.axis.unhomed {
  border-style: dashed;
  border-color: var(--warn);
}

.axis.unhomed .work {
  font-weight: 400;
  font-style: italic;
  color: var(--warn);
}

.axis.unhomed .letter > span:first-child {
  color: var(--warn);
}

/* Not live: every number is history. */
.dro.distrust .axis {
  border-style: dotted;
  border-color: var(--line-strong);
}

.dro.distrust .axis .work,
.dro.distrust .axis .detail,
.dro.distrust .axis .letter > span:first-child {
  color: var(--text-faint);
  text-decoration: line-through;
  text-decoration-thickness: 1px;
}

.distrust .state,
.distrust .facts,
.distrust .codes {
  opacity: 0.45;
}

.touch {
  flex: none;
}

.facts {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
}

.state {
  display: flex;
  flex-direction: column;
  gap: 2px;
  padding: 8px 12px;
  border: 1px solid var(--line);
  border-left-width: 4px;
  border-radius: var(--radius);
}

.state.warn {
  border-color: var(--warn);
  background: var(--warn-soft);
}

.state.go {
  border-left-color: var(--go);
}

.state .title {
  font-size: 17px;
  font-weight: 600;
}

.state .detail {
  min-height: 17px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.codes {
  display: flex;
  flex-direction: column;
  gap: 2px;
}

.codes .list {
  font-size: 13px;
  color: var(--text-dim);
}
</style>
