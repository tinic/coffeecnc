<script setup lang="ts">
import { vTooltip } from '../tooltip'
import { computed, ref, watch } from 'vue'
import { startDialog } from '../dialogs'
import { axes, live, mdiBlock, panel, reported, send } from '../machine'
import type { AxisLetter, AxisStatus } from '../protocol'
import { formatLength, fromDisplay, wcsName } from '../units'
import AppIcon from './AppIcon.vue'
import NumberDialog from './NumberDialog.vue'

const basis = ref<'work' | 'machine' | 'dtg'>('work')
const unit = computed(() => panel.unit)
const wcs = computed(() => wcsName(reported.value?.offsets.wcs ?? 1))
const touching = ref<{ key: number; axis: AxisLetter; initial: string; millimeters: number } | null>(null)
let nextTouch = 0
const touchBlock = computed(() => panel.tab !== 'manual' ? 'Open Manual to touch off'
  : basis.value !== 'work' ? 'Switch to work position to touch off' : mdiBlock.value)
const shifted = computed(() => reported.value?.offsets.g92.some((v) => v !== 0) ?? false)
const tool = computed(() => reported.value?.tool.in_spindle)
const compensation = computed(() => reported.value?.active_codes.find((code) => /^G(?:43(?:\.[12])?|49)$/.test(code)) ?? '—')
const toolLength = computed(() => reported.value?.offsets.tool[2])
function cycleBasis(): void { basis.value = basis.value === 'work' ? 'machine' : basis.value === 'machine' ? 'dtg' : 'work' }
function marker(axis: AxisStatus): string {
  if (axis.homing) return 'Homing'
  if (axis.on_hard_limit) return 'On limit'
  return axis.homed ? '' : 'Not homed'
}
function openTouchOff(axis: AxisStatus): void {
  if (touchBlock.value !== null || !Number.isFinite(axis.work)) return
  touching.value = { key: ++nextTouch, axis: axis.axis, initial: formatLength(axis.work, unit.value), millimeters: axis.work }
}
function touchOff(value: number, text: string): void {
  const draft = touching.value
  if (draft !== null && touchBlock.value === null) {
    // Accepting the offered coordinate must not round the existing work offset.
    const millimeters = text === draft.initial ? draft.millimeters : fromDisplay(value, unit.value)
    send({ type: 'touch_off', axis: draft.axis, value: millimeters })
  }
  touching.value = null
}
watch([(): unknown => panel.hello, (): string => panel.tab, unit, basis, wcs, touchBlock], () => { touching.value = null }, { flush: 'sync' })
let previewRequested = startDialog === 'touchoff'
watch([axes, touchBlock], () => {
  const axis = axes.value.find((candidate) => candidate.axis === 'x')
  if (previewRequested && axis && touchBlock.value === null) { previewRequested = false; openTouchOff(axis) }
}, { immediate: true })
</script>

<template>
  <section class="dro" :class="{ distrust: !live }" aria-label="Coordinate readout">
    <button v-tooltip="'Switch between work position, machine position and distance to go'" class="basis" type="button" @click="cycleBasis">
      <strong>{{ basis === 'work' ? wcs : basis === 'machine' ? 'Machine' : 'To go' }}</strong>
      <span>{{ basis === 'work' ? 'Work position' : basis === 'machine' ? 'Position' : 'Remaining' }}<template v-if="shifted && basis === 'work'"> · G92</template></span>
    </button>
    <button v-for="axis in axes" :key="axis.axis" v-tooltip="touchBlock ?? `Touch off ${axis.axis.toUpperCase()} in ${wcs}`" class="axis" type="button" :disabled="touchBlock !== null || !Number.isFinite(axis.work)" :aria-label="`Touch off ${axis.axis.toUpperCase()} in ${wcs}`" @click="openTouchOff(axis)">
      <span class="letter mono">{{ axis.axis.toUpperCase() }}</span>
      <span class="reading"><span class="number mono">{{ formatLength(axis[basis], unit) }}</span><span class="marker">{{ !live ? 'Last reported' : marker(axis) }}</span></span>
      <span class="touch" :class="{ hidden: panel.tab !== 'manual' }" aria-hidden="true"><AppIcon name="target" /></span>
    </button>
    <span v-if="axes.length === 0" class="unavailable">Position unavailable</span>
    <div class="units" role="group" aria-label="Display units">
      <button type="button" :aria-pressed="unit === 'mm'" @click="panel.unit = 'mm'">mm</button>
      <button type="button" :aria-pressed="unit === 'inch'" @click="panel.unit = 'inch'">in</button>
    </div>
    <div v-if="panel.tab === 'manual'" class="active-tool" :class="{ stale: !live }" aria-label="Active tool and length compensation">
      <span class="tool-heading">{{ live ? 'Active tool' : 'Last reported tool' }}</span>
      <strong class="mono">{{ tool === undefined ? '—' : tool === 0 ? 'T0 · No tool' : `T${tool}` }} <span v-if="tool !== undefined">· {{ compensation }}</span></strong>
      <span class="tool-length">Z length <span class="mono">{{ toolLength === undefined ? '—' : formatLength(toolLength, unit) }}</span> {{ unit === 'inch' ? 'in' : 'mm' }}</span>
    </div>
    <NumberDialog v-if="touching !== null" :key="touching.key" :title="`Touch off ${touching.axis.toUpperCase()} in ${wcs}`" :effect="`Changes the ${wcs} work offset so that ${touching.axis.toUpperCase()} reads the value below at the present position. The machine does not move.`" :label="`${touching.axis.toUpperCase()} is now at`" :unit="unit" :initial="touching.initial" :action="`Set ${wcs} ${touching.axis.toUpperCase()}`" @confirm="touchOff" @close="touching = null" />
  </section>
</template>

<style scoped>
.dro { display: flex; align-items: center; flex: none; min-height: 78px; margin: 0 24px 10px; padding: 12px 0; border: 1px solid var(--line); border-radius: 12px; background: var(--panel); }
.basis { display: flex; flex-direction: column; gap: 5px; width: 125px; flex: none; padding: 0 20px; border: 0; background: none; text-align: left; cursor: pointer; }
.basis strong { font-size: 13px; font-weight: 500; }.basis span { font-size: 10px; color: var(--text-dim); white-space: nowrap; }
.axis { display: flex; flex: 0 1 250px; height: 48px; align-items: center; gap: 12px; min-width: 0; padding: 0 18px; border: 0; border-left: 1px solid var(--line); background: none; color: var(--text); text-align: left; cursor: pointer; }
.axis:disabled { cursor: default; }.axis:hover:not(:disabled) { background: var(--accent-soft); }.axis:focus-visible { outline: 2px solid var(--accent); outline-offset: -2px; border-radius: 4px; }
.letter { flex: none; width: 20px; color: var(--text); }.reading { position: relative; display: flex; flex: 1; flex-direction: column; min-width: 0; text-align: right; }
.number, .letter { font-size: 27px; line-height: 1.3; letter-spacing: -1px; font-weight: 500; white-space: nowrap; }.marker { position: absolute; top: 100%; left: 0; height: 15px; line-height: 15px; white-space: nowrap; font-size: 10px; color: var(--warn); }
.touch { display: flex; align-self: center; padding: 8px; color: var(--text-dim); }.axis:disabled .touch { opacity: .3; }
.touch.hidden { visibility: hidden; }
.units { display: flex; flex: none; gap: 3px; padding: 3px; margin: 0 12px; border: 1px solid var(--line-strong); border-radius: 9px; background: var(--bg-sunken); }
.units button { width: 52px; height: 48px; border: 1px solid transparent; border-radius: 6px; background: none; font-size: 16px; color: var(--text-dim); cursor: pointer; }
.units button[aria-pressed="true"] { background: var(--accent-soft); border-color: var(--accent); color: var(--text); font-weight: 600; }
.active-tool { display: flex; flex: 0 0 174px; flex-direction: column; justify-content: center; align-self: stretch; gap: 3px; margin-left: auto; padding: 0 16px; border-left: 1px solid var(--line); white-space: nowrap; }
.tool-heading, .tool-length { color: var(--text-dim); font-size: 11px; }.active-tool strong { font-size: 14px; font-weight: 500; }.active-tool.stale strong { color: var(--text-faint); }
.distrust .number { color: var(--text-faint); }.unavailable { flex: 1; color: var(--text-dim); }
@media (max-height: 820px) { .dro { min-height: 70px; padding-block: 8px; margin-bottom: 8px; } }
</style>
