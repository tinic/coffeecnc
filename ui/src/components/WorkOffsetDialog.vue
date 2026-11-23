<script setup lang="ts">
import { computed, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { translateMessage } from '../i18n/messages'
import { mdiBlock, panel, send } from '../machine'
import { parseNumericExpression } from '../numeric-expression'
import { vNumeric } from '../numeric-input'
import type { Vec3 } from '../protocol'
import { formatLength, parseLength, toDisplay, wcsName, wcsNames } from '../units'
import { workOffsetRows } from '../work-offsets'
import ModalDialog from './ModalDialog.vue'

const props = defineProps<{ wcs: number }>()
const emit = defineEmits<{ close: [] }>()
const { t } = useI18n()
const unit = panel.unit
const unitName = unit === 'inch' ? 'in' : 'mm'
const hello = panel.hello
const axes = ['X', 'Y', 'Z'] as const
/** The largest XY rotation either way, in degrees, as the native backend accepts it. */
const maxRotation = 360
function current(wcs: number): Vec3 | null {
  const row = workOffsetRows(panel.workOffsets, panel.status, panel.link).find((candidate) => candidate.wcs === wcs)
  return row?.offset === null || row?.offset === undefined ? null : [...row.offset]
}
function equal(a: Vec3 | null, b: Vec3 | null): boolean {
  return a === null || b === null ? a === b : a.every((value, index) => value === b[index])
}
const original = current(props.wcs)
const initial = original?.map((value) => String(Number(toDisplay(value, unit).toFixed(6)))) ?? ['', '', '']
const values = ref([...initial])
const destination = ref(props.wcs === 1 ? 2 : 1)
const parsed = computed((): Vec3 | null => {
  const numbers = values.value.map((text, index) => original !== null && text === initial[index]
    ? original[index] ?? null : parseLength(text, unit))
  return numbers.every((value) => value !== null && Number.isFinite(value)) ? numbers as Vec3 : null
})
const block = computed(() => mdiBlock.value !== null ? translateMessage(mdiBlock.value)
  : panel.status?.link !== 'live' || panel.link !== 'live' ? t('probe.workOffset.waitStatus')
    : panel.hello !== hello ? t('probe.workOffset.connectionChanged')
      : !equal(current(props.wcs), original) ? t('probe.workOffset.originsChanged') : null)
// The XY rotation of this system, in degrees, edited and zeroed on its own (G10 L2 R).
function rotation(wcs: number): number | null {
  return workOffsetRows(panel.workOffsets, panel.status, panel.link).find((candidate) => candidate.wcs === wcs)?.rotation_xy ?? null
}
const originalRotation = rotation(props.wcs)
const initialRotation = originalRotation === null ? '' : String(Number(originalRotation.toFixed(6)))
const rotationText = ref(initialRotation)
const parsedRotation = computed((): number | null => {
  const value = originalRotation !== null && rotationText.value === initialRotation ? originalRotation
    : parseNumericExpression(rotationText.value, '°')
  return value !== null && Number.isFinite(value) && Math.abs(value) <= maxRotation ? value : null
})
// The G92/G52 shift: one offset of every system, cleared with G92.1.
function shift(): Vec3 | null {
  const status = panel.status
  if (status?.link !== 'live' || panel.link !== 'live') return null
  return [...status.offsets.g92]
}
const shifted = computed(() => shift()?.some((value) => value !== 0) ?? false)
interface OriginReview { kind: 'set' | 'copy' | 'clear'; wcs: number; before: Vec3 | null; target: Vec3 }
interface RotationReview { kind: 'rotation' | 'zeroRotation'; wcs: number; before: number | null; target: number }
interface ShiftReview { kind: 'shift'; before: Vec3; target: Vec3 }
type Review = OriginReview | RotationReview | ShiftReview
function turns(now: Review): now is RotationReview { return now.kind === 'rotation' || now.kind === 'zeroRotation' }
function shifts(now: Review): now is ShiftReview { return now.kind === 'shift' }
const review = ref<Review | null>(null)
// What the review dialog shows: the rotation, or XYZ (of a system or of the shift).
const turning = computed(() => review.value !== null && turns(review.value) ? review.value : null)
const moving = computed(() => review.value !== null && !turns(review.value) ? review.value : null)
const reviewBlock = computed(() => {
  const now = review.value
  if (block.value !== null || now === null) return block.value
  if (shifts(now)) return equal(shift(), now.before) ? null : t('probe.workOffset.shiftChanged')
  if (turns(now)) return rotation(now.wcs) === now.before ? null : t('probe.workOffset.rotationChanged')
  return equal(current(now.wcs), now.before) ? null : t('probe.workOffset.destinationChanged')
})
const reviewWcs = computed(() => review.value === null || shifts(review.value) ? '' : wcsName(review.value.wcs))
const reviewActive = computed(() => review.value !== null && !shifts(review.value) && panel.status?.offsets.wcs === review.value.wcs)
const reviewTitle = computed(() => {
  const now = review.value
  if (now === null) return ''
  const wcs = reviewWcs.value
  switch (now.kind) {
    case 'shift': return t('probe.workOffset.shiftTitle')
    case 'copy': return t('probe.workOffset.copyTitle', { wcs })
    case 'clear': return t('probe.workOffset.clearTitle', { wcs })
    case 'rotation': return t('probe.workOffset.rotationTitle', { wcs })
    case 'zeroRotation': return t('probe.workOffset.zeroRotationTitle', { wcs })
    case 'set': return t('probe.workOffset.setTitle', { wcs })
  }
})
const confirmLabel = computed(() => review.value === null ? ''
  : shifts(review.value) ? t('probe.workOffset.confirmShift')
    : turns(review.value) ? t('probe.workOffset.confirmRotation', { wcs: reviewWcs.value })
      : t('probe.workOffset.confirm', { wcs: reviewWcs.value }))
function prepare(kind: 'set' | 'copy' | 'clear'): void {
  const target: Vec3 | null = kind === 'clear' ? [0, 0, 0] : parsed.value
  if (block.value !== null || target === null) return
  const wcs = kind === 'copy' ? destination.value : props.wcs
  review.value = { kind, wcs, before: current(wcs), target: [...target] }
}
function prepareRotation(kind: 'rotation' | 'zeroRotation'): void {
  const target = kind === 'zeroRotation' ? 0 : parsedRotation.value
  if (block.value !== null || target === null) return
  review.value = { kind, wcs: props.wcs, before: rotation(props.wcs), target }
}
function prepareShift(): void {
  const before = shift()
  if (block.value !== null || before === null) return
  review.value = { kind: 'shift', before, target: [0, 0, 0] }
}
function confirm(): void {
  const now = review.value
  if (now === null || reviewBlock.value !== null) return
  if (shifts(now)) send({ type: 'clear_g92' })
  else if (turns(now)) send({ type: 'set_work_rotation', wcs: now.wcs, rotation_xy: now.target })
  else send({ type: 'set_work_offset', wcs: now.wcs, offset: [...now.target] })
  emit('close')
}
function degrees(value: number | null): string {
  return value === null ? t('common.unavailable') : `${String(Number(value.toFixed(6)))}°`
}
// Position readouts are intentionally absent: encoder feedback does not touch this draft.
watch([(): unknown => panel.hello, (): string => panel.unit], () => { emit('close'); }, { flush: 'sync' })
watch([mdiBlock, (): number | undefined => panel.status?.offsets.wcs], () => { review.value = null }, { flush: 'sync' })
</script>

<template>
  <ModalDialog v-if="review !== null" key="review" :title="reviewTitle" size="medium" expanded fresh-gesture @close="review = null">
    <template v-if="turning !== null">
      <table class="changes">
        <thead><tr><th></th><th>{{ t('probe.workOffset.current', { unit: '°' }) }}</th><th>{{ t('probe.workOffset.new', { unit: '°' }) }}</th></tr></thead>
        <tbody><tr><th>{{ t('probe.workOffset.rotation') }}</th><td class="mono">{{ degrees(turning.before) }}</td><td class="mono">{{ degrees(turning.target) }}</td></tr></tbody>
      </table>
      <p>{{ t('probe.workOffset.rotationEffect', { wcs: reviewWcs }) }}</p>
      <p v-if="reviewActive" class="hint">{{ t('probe.workOffset.activeRotation', { wcs: reviewWcs }) }}</p>
    </template>
    <template v-else-if="moving !== null">
      <table class="changes">
        <thead><tr><th>{{ t('probe.workOffset.axis') }}</th><th>{{ t('probe.workOffset.current', { unit: unitName }) }}</th><th>{{ t('probe.workOffset.new', { unit: unitName }) }}</th></tr></thead>
        <tbody><tr v-for="(axis, index) in axes" :key="axis"><th class="mono">{{ axis }}</th><td class="mono">{{ moving.before === null ? t('common.unavailable') : formatLength(moving.before[index]!, unit) }}</td><td class="mono">{{ formatLength(moving.target[index]!, unit) }}</td></tr></tbody>
      </table>
      <template v-if="moving.kind === 'shift'"><p>{{ t('probe.workOffset.shiftEffect') }}</p><p class="hint">{{ t('probe.workOffset.shiftUndo') }}</p></template>
      <template v-else>
        <p>{{ t('probe.workOffset.replaces', { wcs: reviewWcs }) }}</p>
        <p v-if="reviewActive" class="hint">{{ t('probe.workOffset.active', { wcs: reviewWcs }) }}</p>
      </template>
    </template>
    <p v-if="reviewBlock" class="blocked" role="status">{{ reviewBlock }}</p>
    <template #footer><button class="btn" type="button" @click="review = null">{{ t('common.back') }}</button><button class="btn primary" type="button" :disabled="reviewBlock !== null" @click="confirm">{{ confirmLabel }}</button></template>
  </ModalDialog>
  <ModalDialog v-else key="edit" :title="t('probe.workOffset.title', { wcs: wcsName(wcs) })" size="medium" expanded @close="emit('close')">
    <p class="hint">{{ t('probe.workOffset.hint', { unit: unitName }) }}</p>
    <p v-if="original === null" class="hint">{{ t('probe.workOffset.unknown') }}</p>
    <div class="coordinates">
      <label v-for="(axis, index) in axes" :key="axis" class="entry"><span class="mono">{{ axis }}</span><input v-model="values[index]" v-numeric :data-numeric-unit="unit" :aria-label="t('probe.workOffset.originLabel', { wcs: wcsName(wcs), axis })" class="field mono" type="text" inputmode="decimal" autocomplete="off" :placeholder="original === null ? t('common.unavailable') : ''" /></label>
    </div>
    <div class="offset-actions">
      <button class="btn" type="button" :disabled="block !== null" @click="prepare('clear')">{{ t('probe.workOffset.clear') }}</button>
      <span class="spacer"></span>
      <label class="copy-target"><span>{{ t('probe.workOffset.copyTo') }}</span><select v-model="destination" class="field mono" :aria-label="t('probe.workOffset.copyDestination')"><option v-for="(name, index) in wcsNames" :key="name" :value="index + 1" :disabled="index + 1 === wcs">{{ name }}</option></select></label>
      <button class="btn" type="button" :disabled="block !== null || parsed === null" @click="prepare('copy')">{{ t('probe.workOffset.copy') }}</button>
    </div>
    <div class="offset-actions rotation">
      <label class="inline-entry"><span>{{ t('probe.workOffset.rotation') }}</span><input v-model="rotationText" v-numeric data-numeric-unit="°" :aria-label="t('probe.workOffset.rotationLabel', { wcs: wcsName(wcs) })" class="field mono" type="text" inputmode="decimal" autocomplete="off" :placeholder="originalRotation === null ? t('common.unavailable') : ''" /><span class="unit">°</span></label>
      <span class="spacer"></span>
      <button class="btn" type="button" :disabled="block !== null || originalRotation === 0" @click="prepareRotation('zeroRotation')">{{ t('probe.workOffset.zeroRotation') }}</button>
      <button class="btn" type="button" :disabled="block !== null || parsedRotation === null || parsedRotation === originalRotation" @click="prepareRotation('rotation')">{{ t('probe.workOffset.setRotation') }}</button>
    </div>
    <div v-if="shifted" class="offset-actions shift">
      <span class="shift-values"><span>{{ t('probe.workOffset.shift') }}</span><span class="mono shift-axes"><span v-for="(value, index) in shift() ?? []" :key="index">{{ axes[index] }} {{ formatLength(value, unit) }}</span><span>{{ unitName }}</span></span></span>
      <span class="spacer"></span>
      <button class="btn" type="button" :disabled="block !== null" @click="prepareShift">{{ t('probe.workOffset.clearShift') }}</button>
    </div>
    <p v-if="block" class="blocked" role="status">{{ block }}</p>
    <template #footer><button class="btn" type="button" @click="emit('close')">{{ t('common.cancel') }}</button><button class="btn primary" type="button" :disabled="block !== null || parsed === null" @click="prepare('set')">{{ t('probe.workOffset.review') }}</button></template>
  </ModalDialog>
</template>

<style scoped>
p { margin: 0; line-height: 1.45; }
.coordinates { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 12px; }
.entry { display: flex; flex-direction: column; gap: 8px; font-size: 18px; }.entry input { width: 100%; min-width: 0; min-height: 48px; font-size: 20px; }
.offset-actions, .copy-target { display: flex; align-items: center; gap: 8px; }.offset-actions { padding-top: 12px; border-top: 1px solid var(--line); }.copy-target { font-size: 13px; }.copy-target select { min-height: 44px; }
.spacer { flex: 1; }.btn { min-height: 44px; }
.inline-entry { display: flex; align-items: center; gap: 8px; font-size: 13px; }.inline-entry input { width: 120px; min-height: 44px; font-size: 18px; }.inline-entry .unit { font-size: 16px; }
.shift-values { display: flex; flex-direction: column; gap: 2px; font-size: 13px; color: var(--warn); }.shift-axes { display: flex; flex-wrap: wrap; gap: 12px; font-size: 14px; }
.changes { width: 100%; border-collapse: collapse; }.changes th, .changes td { padding: 10px 8px; border-bottom: 1px solid var(--line); text-align: right; }.changes th:first-child { text-align: left; }.changes thead { color: var(--text-dim); font-size: 12px; }.changes td { font-size: 19px; }
</style>
