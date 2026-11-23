<script setup lang="ts">
import { vTooltip } from '../tooltip'
import { vPopover } from '../popover'
import { computed, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { startDialog } from '../dialogs'
import { translateMessage } from '../i18n/messages'
import { axes, live, mdiBlock, panel, reported, rotaryAxes, send } from '../machine'
import type { AxisLetter, AxisStatus, RotaryAxisStatus } from '../protocol'
import { formatLength, fromDisplay, wcsName } from '../units'
import AppIcon from './AppIcon.vue'
import NumberDialog from './NumberDialog.vue'
import CoordinateConfirm from './CoordinateConfirm.vue'

const { t } = useI18n()
const props = defineProps<{ readOnly?: boolean }>()
const basis = ref<'work' | 'machine' | 'dtg'>('work')
const unit = computed(() => panel.unit)
const wcs = computed(() => wcsName(reported.value?.offsets.wcs ?? 1))
const touching = ref<{ key: number; axis: AxisLetter; initial: string; millimeters: number } | null>(null)
const quick = ref<{ axis: AxisLetter; operation: 'zero' | 'half' } | null>(null)
const quickValues = computed(() => {
  const draft = quick.value
  const current = axes.value.find((axis) => axis.axis === draft?.axis)?.work
  if (!draft || current === undefined || !Number.isFinite(current)) return null
  return { current, target: draft.operation === 'half' ? current / 2 : 0 }
})
const basisLabel = computed(() => basis.value === 'work' ? t('manual.dro.basisWork', { wcs: wcs.value })
  : basis.value === 'machine' ? t('manual.dro.basisMachine') : t('manual.dro.basisToGo'))
const nextBasisLabel = computed(() => basis.value === 'work' ? t('manual.dro.basisMachine')
  : basis.value === 'machine' ? t('manual.dro.basisToGo') : t('manual.dro.nextWork'))
const switchTip = computed(() => basis.value === 'work' ? t('manual.dro.switchToMachine')
  : basis.value === 'machine' ? t('manual.dro.switchToToGo') : t('manual.dro.switchToWork'))
function cycleBasis(): void {
  basis.value = basis.value === 'work' ? 'machine' : basis.value === 'machine' ? 'dtg' : 'work'
}
let nextTouch = 0
const touchBlock = computed(() => props.readOnly || panel.tab !== 'manual' ? t('manual.dro.openManual')
  : basis.value !== 'work' ? t('manual.dro.switchToWorkPosition') : mdiBlock.value === null ? null : translateMessage(mdiBlock.value))
const shifted = computed(() => reported.value?.offsets.g92.some((v) => v !== 0) ?? false)
const tool = computed(() => reported.value?.tool.in_spindle)
// A longer translation ends in an ellipsis in the fixed column; the tooltip has all of it.
const toolHeading = computed(() => !live.value ? t('manual.dro.lastReportedTool') : tool.value === 0 ? t('manual.dro.noToolLoaded') : t('manual.dro.activeTool'))
const compensation = computed(() => reported.value?.active_codes.find((code) => /^G(?:43(?:\.[12])?|49)$/.test(code)) ?? '—')
const toolLength = computed(() => reported.value?.offsets.tool[2])
function openQuick(axis: AxisStatus, operation: 'zero' | 'half'): void {
  if (touchBlock.value !== null || !Number.isFinite(axis.work)) return
  touching.value = null
  quick.value = { axis: axis.axis, operation }
}
function quickFromMenu(axis: AxisStatus, operation: 'zero' | 'half', event: MouseEvent): void {
  const menu = (event.currentTarget as HTMLElement).closest('details')
  if (menu) menu.open = false
  openQuick(axis, operation)
}
function confirmQuick(): void {
  const draft = quick.value
  const values = quickValues.value
  if (draft && values && touchBlock.value === null) {
    send({ type: 'touch_off', axis: draft.axis, value: values.target, origin: draft.operation === 'half' ? 'half' : 'zero' })
  }
  quick.value = null
}
function marker(axis: AxisStatus | RotaryAxisStatus): string {
  if (axis.homing) return t('manual.dro.homing')
  if (axis.on_hard_limit) return t('manual.dro.onLimit')
  return axis.homed ? '' : t('manual.dro.notHomed')
}
function formatAngle(degrees: number | null): string {
  if (degrees === null || !Number.isFinite(degrees)) return '—'
  const value = degrees.toFixed(3)
  return `${value === '-0.000' ? '0.000' : value}°`
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
  <section class="dro" :class="{ distrust: !live, 'has-rotary': rotaryAxes.length > 0 }" :aria-label="t('manual.dro.readout')">
    <div class="basis">
      <button v-tooltip="switchTip" class="btn basis-toggle" type="button" :aria-label="t('manual.dro.view', { current: basisLabel, next: nextBasisLabel })" @click="cycleBasis">
        <span>{{ basisLabel }}<small v-if="basis === 'work' && shifted" class="g92">{{ t('manual.dro.shiftActive') }}</small></span>
        <AppIcon name="forward" />
      </button>
    </div>
    <div v-for="axis in axes" :key="axis.axis" class="axis-group">
      <component
        :is="readOnly ? 'div' : 'button'"
        v-tooltip="readOnly ? undefined : touchBlock ?? t('manual.dro.touchOff', { axis: axis.axis.toUpperCase(), wcs })"
        class="axis tone-control" :class="{ 'remote-axis': readOnly }"
        :type="readOnly ? undefined : 'button'" :disabled="readOnly ? undefined : touchBlock !== null || !Number.isFinite(axis.work)"
        :aria-label="readOnly ? undefined : t('manual.dro.touchOff', { axis: axis.axis.toUpperCase(), wcs })" @click="openTouchOff(axis)"
      >
        <span class="letter mono">{{ axis.axis.toUpperCase() }}</span>
        <span class="reading"><span class="number mono">{{ formatLength(axis[basis], unit) }}</span><span class="marker">{{ !live ? t('manual.dro.lastReported') : marker(axis) }}</span></span>
      </component>
      <details v-if="!readOnly && panel.tab === 'manual'" v-popover class="axis-actions">
        <summary class="tone-control" :aria-label="t('manual.dro.axisActions', { axis: axis.axis.toUpperCase() })">···</summary>
        <div class="popover">
          <h3>{{ axis.axis.toUpperCase() }} · {{ wcs }}</h3>
          <button class="btn" type="button" :disabled="touchBlock !== null || !Number.isFinite(axis.work)" :aria-label="t('manual.dro.zeroAxis', { axis: axis.axis.toUpperCase() })" @click="quickFromMenu(axis, 'zero', $event)">{{ t('manual.dro.zeroAxis', { axis: axis.axis.toUpperCase() }) }}</button>
          <button class="btn" type="button" :disabled="touchBlock !== null || !Number.isFinite(axis.work)" :aria-label="t('manual.dro.halfAxis', { axis: axis.axis.toUpperCase() })" @click="quickFromMenu(axis, 'half', $event)">{{ t('manual.dro.halfAxisButton', { axis: axis.axis.toUpperCase() }) }}</button>
        </div>
      </details>
    </div>
    <span v-if="axes.length === 0" class="unavailable">{{ t('manual.dro.positionUnavailable') }}</span>
    <div class="display-units">
      <div class="units" role="group" :aria-label="t('manual.dro.displayUnits')">
        <button class="tone-control" type="button" :aria-pressed="unit === 'mm'" @click="panel.unit = 'mm'">mm</button>
        <button class="tone-control" type="button" :aria-pressed="unit === 'inch'" @click="panel.unit = 'inch'">in</button>
      </div>
    </div>
    <div class="active-tool" :class="{ stale: !live }" :aria-label="t('manual.dro.activeToolGroup')">
      <span v-tooltip="toolHeading" class="tool-heading">{{ toolHeading }}</span>
      <strong class="mono">{{ tool === undefined ? '—' : `T${tool}` }} <span v-if="tool !== undefined">· {{ compensation }}</span></strong>
      <span class="tool-length">{{ t('manual.dro.length') }} <span class="mono">{{ toolLength === undefined ? '—' : formatLength(toolLength, unit) }}</span> {{ unit === 'inch' ? 'in' : 'mm' }}</span>
    </div>
    <div v-if="rotaryAxes.length" class="rotary" role="group" :aria-label="t('manual.dro.rotary')">
      <div v-for="axis in rotaryAxes" :key="axis.axis" class="rotary-axis">
        <span class="rotary-letter mono">{{ axis.axis.toUpperCase() }}</span>
        <span class="rotary-angle mono">{{ formatAngle(axis[basis]) }}</span>
        <span v-if="live && marker(axis)" class="rotary-marker">{{ marker(axis) }}</span>
      </div>
      <span v-if="!live" class="rotary-marker rotary-stale">{{ t('manual.dro.lastReported') }}</span>
    </div>
    <NumberDialog v-if="touching !== null" :key="touching.key" :title="t('manual.dro.touchOff', { axis: touching.axis.toUpperCase(), wcs })" :effect="t('manual.dro.touchOffEffect', { wcs, axis: touching.axis.toUpperCase() })" :label="t('manual.dro.nowAt', { axis: touching.axis.toUpperCase() })" :unit="unit" :initial="touching.initial" :action="t('manual.dro.setAxis', { wcs, axis: touching.axis.toUpperCase() })" @confirm="touchOff" @close="touching = null" />
    <CoordinateConfirm v-if="quick && quickValues" :axis="quick.axis" :wcs="wcs" :current="quickValues.current" :target="quickValues.target" :unit="unit" :operation="quick.operation" @confirm="confirmQuick" @close="quick = null" />
  </section>
</template>

<style scoped>
.dro { --basis-width: clamp(154px, 13.75vw, 176px); display: flex; align-items: center; flex: none; min-height: 78px; margin: 0 24px 10px; padding: 12px 0; border-radius: var(--radius); background: var(--panel); }
.dro.has-rotary { flex-wrap: wrap; }
.rotary { display: flex; flex: 0 0 100%; align-items: center; margin-top: 6px; padding: 4px 0 0 var(--basis-width); border-top: 1px solid var(--line); }
.rotary-axis { display: grid; grid-template-columns: 20px 132px minmax(0, 1fr); flex: 0 0 257px; align-items: baseline; gap: 12px; padding: 0 12px; border-left: 1px solid var(--line); white-space: nowrap; }
.rotary-letter, .rotary-angle { font-size: var(--type-lg); line-height: 24px; font-weight: 500; }
.rotary-angle { text-align: right; }
.rotary-marker { color: var(--warn); font-size: var(--type-sm); }
.rotary-stale { margin-left: auto; padding: 0 12px; white-space: nowrap; }
.distrust .rotary-angle { color: var(--text-faint); }
.basis { display: flex; align-self: stretch; width: var(--basis-width); flex: none; padding: 0 10px; }
.basis-toggle { width: 100%; min-height: 56px; justify-content: space-between; gap: 8px; padding: 0 10px; font-size: var(--type-body); font-weight: 600; white-space: nowrap; }
.basis-toggle > span { min-width: 0; white-space: normal; line-height: 1.2; text-align: left; }
.basis-toggle :deep(svg) { width: 16px; height: 16px; flex: none; color: var(--text-dim); }
.g92 { display: block; font-size: var(--type-sm); line-height: 1.2; color: var(--warn); }
.axis-group { display: flex; align-items: center; justify-content: center; flex: 1 1 0; min-width: 0; border-left: 1px solid var(--line); }
.axis { display: flex; height: 56px; align-items: center; justify-content: flex-end; gap: 9px; width: 11.8ch; min-width: 0; padding: 0 8px; border: 0; border-radius: 7px; background: none; color: var(--text); font: 500 var(--type-dro)/1.2 var(--mono); cursor: pointer; }
.axis-actions { position: relative; flex: none; margin-right: 4px; }.axis-actions summary { display: grid; place-items: center; width: 44px; height: 44px; border-radius: 7px; color: var(--text-dim); font-size: 20px; }.axis-actions summary:hover { background: var(--accent-soft); }.axis-actions .popover { top: calc(100% + 8px); right: 0; width: 190px; min-width: 190px; }.axis-actions .btn { width: 100%; justify-content: flex-start; margin-top: 6px; }
.axis:disabled, .axis.remote-axis { cursor: default; }.axis:hover:not(:disabled, .remote-axis) { background: var(--accent-soft); }.axis:focus-visible { outline: 2px solid var(--accent); outline-offset: -2px; border-radius: 4px; }
.letter { flex: none; }
/* The readout ends at a fixed position while the axis letter sits next to the first digit. */
.reading { position: relative; display: flex; flex: none; flex-direction: column; text-align: right; }
.number, .letter { letter-spacing: -.025em; white-space: nowrap; }
.marker { position: absolute; top: 100%; right: 0; line-height: 1.2; white-space: nowrap; font: 600 var(--type-sm)/1.2 var(--sans); color: var(--warn); }
.units { display: flex; flex: none; gap: 3px; padding: 3px; border-radius: 9px; background: var(--bg-sunken); }
.display-units { display: flex; flex: none; align-items: center; gap: 8px; margin: 0 12px; padding-left: 12px; border-left: 1px solid var(--line); }
.units button { width: 44px; height: 48px; border: 0; border-radius: 6px; background: none; font-size: var(--type-md); color: var(--text-dim); cursor: pointer; }
.units button[aria-pressed="true"] { background: var(--accent-soft); color: var(--accent); font-weight: 650; }
.active-tool { display: flex; flex: 0 0 158px; flex-direction: column; justify-content: center; align-self: stretch; gap: 3px; margin-left: auto; padding: 0 12px; border-left: 1px solid var(--line); white-space: nowrap; }
.tool-heading, .tool-length { color: var(--text-dim); font-size: var(--type-sm); }.tool-heading { overflow: hidden; text-overflow: ellipsis; }.active-tool strong { font-size: var(--type-md); font-weight: 500; }.active-tool.stale strong { color: var(--text-faint); }
.distrust .number { color: var(--text-faint); }.unavailable { flex: 1; color: var(--text-dim); }
@media (max-height: 820px) { .dro { min-height: 70px; padding-block: 8px; margin-bottom: 8px; } }
@media (max-height: 760px) { .dro { padding-block: 4px; }.axis { height: 52px; }.units button { height: 44px; }.basis-toggle { min-height: 52px; } }
</style>
