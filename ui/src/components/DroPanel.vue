<script setup lang="ts">
import { vTooltip } from '../tooltip'
import { vPopover } from '../popover'
import { computed, ref, watch } from 'vue'
import { startDialog } from '../dialogs'
import { axes, live, mdiBlock, panel, reported, send } from '../machine'
import type { AxisLetter, AxisStatus } from '../protocol'
import { formatLength, fromDisplay, wcsName } from '../units'
import AppIcon from './AppIcon.vue'
import NumberDialog from './NumberDialog.vue'
import CoordinateConfirm from './CoordinateConfirm.vue'

const basis = ref<'work' | 'machine' | 'dtg'>('work')
const unit = computed(() => panel.unit)
const wcs = computed(() => wcsName(reported.value?.offsets.wcs ?? 1))
const touching = ref<{ key: number; axis: AxisLetter; initial: string; millimeters: number } | null>(null)
const quick = ref<{ axis: AxisLetter; operation: 'Zero' | 'Half' } | null>(null)
const quickValues = computed(() => {
  const draft = quick.value
  const current = axes.value.find((axis) => axis.axis === draft?.axis)?.work
  if (!draft || current === undefined || !Number.isFinite(current)) return null
  return { current, target: draft.operation === 'Half' ? current / 2 : 0 }
})
const bases = [['work', 'Work'], ['machine', 'Machine'], ['dtg', 'To go']] as const
let nextTouch = 0
const touchBlock = computed(() => panel.tab !== 'manual' ? 'Open Manual to touch off'
  : basis.value !== 'work' ? 'Switch to work position to touch off' : mdiBlock.value)
const shifted = computed(() => reported.value?.offsets.g92.some((v) => v !== 0) ?? false)
const tool = computed(() => reported.value?.tool.in_spindle)
const compensation = computed(() => reported.value?.active_codes.find((code) => /^G(?:43(?:\.[12])?|49)$/.test(code)) ?? '—')
const toolLength = computed(() => reported.value?.offsets.tool[2])
function openQuick(axis: AxisStatus, operation: 'Zero' | 'Half'): void {
  if (touchBlock.value !== null || !Number.isFinite(axis.work)) return
  touching.value = null
  quick.value = { axis: axis.axis, operation }
}
function quickFromMenu(axis: AxisStatus, operation: 'Zero' | 'Half', event: MouseEvent): void {
  const menu = (event.currentTarget as HTMLElement).closest('details')
  if (menu) menu.open = false
  openQuick(axis, operation)
}
function confirmQuick(): void {
  const draft = quick.value
  const values = quickValues.value
  if (draft && values && touchBlock.value === null) {
    send({ type: 'touch_off', axis: draft.axis, value: values.target })
  }
  quick.value = null
}
function marker(axis: AxisStatus): string {
  if (axis.homing) return 'Homing'
  if (axis.on_hard_limit) return 'On limit'
  return axis.homed ? '' : 'Not homed'
}
function openTouchOff(axis: AxisStatus): void {
  if (touchBlock.value !== null || !Number.isFinite(axis.work)) return
  quick.value = null
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
watch([(): unknown => panel.hello, (): string => panel.tab, unit, basis, wcs, touchBlock], () => { touching.value = null; quick.value = null }, { flush: 'sync' })
// Encoder feedback can fluctuate while stationary. Update the preview without
// interrupting the operator; only loss of a usable coordinate invalidates it.
watch(quickValues, (values) => {
  if (quick.value && values === null) quick.value = null
}, { flush: 'sync' })
let previewRequested = startDialog === 'touchoff'
watch([axes, touchBlock], () => {
  const axis = axes.value.find((candidate) => candidate.axis === 'x')
  if (previewRequested && axis && touchBlock.value === null) { previewRequested = false; openTouchOff(axis) }
}, { immediate: true })
</script>

<template>
  <section class="dro" :class="{ distrust: !live }" aria-label="Coordinate readout">
    <div class="basis">
      <div class="basis-options" role="group" aria-label="Coordinate display">
        <button v-for="[value, label] in bases" :key="value" type="button" :aria-pressed="basis === value" @click="basis = value">{{ label }}<template v-if="value === 'work'"> · {{ wcs }}<small v-if="shifted" class="g92">G92 active</small></template></button>
      </div>
    </div>
    <div v-for="axis in axes" :key="axis.axis" class="axis-group">
      <button v-tooltip="touchBlock ?? `Touch off ${axis.axis.toUpperCase()} in ${wcs}`" class="axis" type="button" :disabled="touchBlock !== null || !Number.isFinite(axis.work)" :aria-label="`Touch off ${axis.axis.toUpperCase()} in ${wcs}`" @click="openTouchOff(axis)">
        <span class="letter mono">{{ axis.axis.toUpperCase() }}</span>
        <span class="reading"><span class="number mono">{{ formatLength(axis[basis], unit) }}</span><span class="marker">{{ !live ? 'Last reported' : marker(axis) }}</span></span>
        <span v-if="panel.tab !== 'manual'" class="touch hidden" aria-hidden="true"><AppIcon name="target" /></span>
      </button>
      <details v-if="panel.tab === 'manual'" v-popover class="axis-actions">
        <summary :aria-label="`${axis.axis.toUpperCase()} coordinate actions`">···</summary>
        <div class="popover">
          <h3>{{ axis.axis.toUpperCase() }} · {{ wcs }}</h3>
          <button class="btn" type="button" :disabled="touchBlock !== null || !Number.isFinite(axis.work)" :aria-label="`Zero ${axis.axis.toUpperCase()}`" @click="quickFromMenu(axis, 'Zero', $event)">Zero {{ axis.axis.toUpperCase() }}</button>
          <button class="btn" type="button" :disabled="touchBlock !== null || !Number.isFinite(axis.work)" :aria-label="`Half ${axis.axis.toUpperCase()}`" @click="quickFromMenu(axis, 'Half', $event)">½ Half {{ axis.axis.toUpperCase() }}</button>
        </div>
      </details>
    </div>
    <span v-if="axes.length === 0" class="unavailable">Position unavailable</span>
    <div class="display-units">
      <div class="units" role="group" aria-label="Display units">
        <button type="button" :aria-pressed="unit === 'mm'" @click="panel.unit = 'mm'">mm</button>
        <button type="button" :aria-pressed="unit === 'inch'" @click="panel.unit = 'inch'">in</button>
      </div>
    </div>
    <div v-if="panel.tab === 'manual'" class="active-tool" :class="{ stale: !live }" aria-label="Active tool and length compensation">
      <span class="tool-heading">{{ !live ? 'Last reported tool' : tool === 0 ? 'No tool loaded' : 'Active tool' }}</span>
      <strong class="mono">{{ tool === undefined ? '—' : `T${tool}` }} <span v-if="tool !== undefined">· {{ compensation }}</span></strong>
      <span class="tool-length">Z length <span class="mono">{{ toolLength === undefined ? '—' : formatLength(toolLength, unit) }}</span> {{ unit === 'inch' ? 'in' : 'mm' }}</span>
    </div>
    <NumberDialog v-if="touching !== null" :key="touching.key" :title="`Touch off ${touching.axis.toUpperCase()} in ${wcs}`" :effect="`Changes the ${wcs} work offset so that ${touching.axis.toUpperCase()} reads the value below at the present position. The machine does not move.`" :label="`${touching.axis.toUpperCase()} is now at`" :unit="unit" :initial="touching.initial" :action="`Set ${wcs} ${touching.axis.toUpperCase()}`" @confirm="touchOff" @close="touching = null" />
    <CoordinateConfirm v-if="quick && quickValues" :axis="quick.axis" :wcs="wcs" :current="quickValues.current" :target="quickValues.target" :unit="unit" :operation="quick.operation" @confirm="confirmQuick" @close="quick = null" />
  </section>
</template>

<style scoped>
.dro { display: flex; align-items: flex-start; flex: none; min-height: 78px; margin: 0 24px 10px; padding: 12px 0; border: 1px solid var(--line); border-radius: 12px; background: var(--panel); }
.basis { display: flex; width: 212px; flex: none; padding: 0 10px; text-align: center; }
.basis-options { display: flex; padding: 3px; gap: 2px; border: 1px solid var(--line-strong); border-radius: 9px; background: var(--bg-sunken); }
.basis-options button { flex: 1; min-height: 48px; padding: 0 7px; border: 1px solid transparent; border-radius: 6px; background: none; color: var(--text-dim); font-size: 12px; cursor: pointer; white-space: nowrap; }
.basis-options button[aria-pressed="true"] { background: var(--accent-soft); border-color: var(--accent); color: var(--text); font-weight: 600; }
.g92 { display: block; font-size: 9px; line-height: 10px; color: var(--warn); }
.axis-group { display: flex; align-items: center; flex: 1 1 250px; min-width: 0; border-left: 1px solid var(--line); }
.axis { display: flex; flex: 1; height: 56px; align-items: center; gap: 10px; min-width: 0; padding: 0 12px; border: 0; background: none; color: var(--text); text-align: left; cursor: pointer; }
.axis-actions { position: relative; flex: none; margin-right: 4px; }.axis-actions summary { display: grid; place-items: center; width: 44px; height: 44px; border-radius: 7px; color: var(--text-dim); font-size: 20px; }.axis-actions summary:hover { background: var(--accent-soft); }.axis-actions .popover { top: calc(100% + 8px); right: 0; width: 190px; min-width: 190px; }.axis-actions .btn { width: 100%; justify-content: flex-start; margin-top: 6px; }
.axis:disabled { cursor: default; }.axis:hover:not(:disabled) { background: var(--accent-soft); }.axis:focus-visible { outline: 2px solid var(--accent); outline-offset: -2px; border-radius: 4px; }
.letter { flex: none; width: 20px; color: var(--text); }.reading { position: relative; display: flex; flex: 1; flex-direction: column; min-width: 0; text-align: right; }
.number, .letter { font-size: 27px; line-height: 1.3; letter-spacing: -1px; font-weight: 500; white-space: nowrap; }.marker { position: absolute; top: 100%; left: 0; height: 15px; line-height: 15px; white-space: nowrap; font-size: 10px; color: var(--warn); }
.touch { display: flex; align-self: center; padding: 2px; color: var(--text-dim); }.axis:disabled .touch { opacity: .3; }
.touch.hidden { visibility: hidden; }
.units { display: flex; flex: none; gap: 3px; padding: 3px; border: 1px solid var(--line-strong); border-radius: 9px; background: var(--bg-sunken); }
.display-units { display: flex; flex: none; align-items: center; gap: 8px; margin: 0 12px; }
.units button { width: 44px; height: 48px; border: 1px solid transparent; border-radius: 6px; background: none; font-size: 16px; color: var(--text-dim); cursor: pointer; }
.units button[aria-pressed="true"] { background: var(--accent-soft); border-color: var(--accent); color: var(--text); font-weight: 600; }
.active-tool { display: flex; flex: 0 0 158px; flex-direction: column; justify-content: center; align-self: stretch; gap: 3px; margin-left: auto; padding: 0 12px; border-left: 1px solid var(--line); white-space: nowrap; }
.tool-heading, .tool-length { color: var(--text-dim); font-size: 11px; }.active-tool strong { font-size: 14px; font-weight: 500; }.active-tool.stale strong { color: var(--text-faint); }
.distrust .number { color: var(--text-faint); }.unavailable { flex: 1; color: var(--text-dim); }
@media (max-height: 820px) { .dro { min-height: 70px; padding-block: 8px; margin-bottom: 8px; } }
@media (max-height: 760px) { .dro { padding-block: 4px; }.axis { height: 52px; }.units button { height: 44px; }.basis-options button { min-height: 44px; } }
</style>
