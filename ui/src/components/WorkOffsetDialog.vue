<script setup lang="ts">
import { computed, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { translateMessage } from '../i18n/messages'
import { mdiBlock, panel, send } from '../machine'
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
function current(wcs: number): Vec3 | null {
  const row = workOffsetRows(panel.workOffsets, panel.status, panel.link).find((candidate) => candidate.wcs === wcs)
  return row?.offset === null || row?.offset === undefined ? null : [...row.offset]
}
function equal(a: Vec3 | null, b: Vec3 | null): boolean {
  return a === null || b === null ? a === b : a.every((value, index) => value === b[index])
}
const unconfirmed = computed(() => workOffsetRows(panel.workOffsets, panel.status, panel.link)
  .find((candidate) => candidate.wcs === props.wcs)?.unconfirmed ?? false)
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
interface Review { kind: 'set' | 'copy' | 'clear'; wcs: number; before: Vec3 | null; target: Vec3 }
const review = ref<Review | null>(null)
const reviewBlock = computed(() => block.value ?? (review.value !== null && !equal(current(review.value.wcs), review.value.before)
  ? t('probe.workOffset.destinationChanged') : null))
const reviewTitle = computed(() => {
  if (review.value === null) return ''
  const wcs = wcsName(review.value.wcs)
  return review.value.kind === 'copy' ? t('probe.workOffset.copyTitle', { wcs })
    : review.value.kind === 'clear' ? t('probe.workOffset.clearTitle', { wcs }) : t('probe.workOffset.setTitle', { wcs })
})
function prepare(kind: Review['kind']): void {
  const target: Vec3 | null = kind === 'clear' ? [0, 0, 0] : parsed.value
  if (block.value !== null || target === null) return
  const wcs = kind === 'copy' ? destination.value : props.wcs
  review.value = { kind, wcs, before: current(wcs), target: [...target] }
}
function confirm(): void {
  if (review.value === null || reviewBlock.value !== null) return
  send({ type: 'set_work_offset', wcs: review.value.wcs, offset: [...review.value.target] })
  emit('close')
}
// Position readouts are intentionally absent: encoder feedback does not touch this draft.
watch([(): unknown => panel.hello, (): string => panel.unit], () => { emit('close'); }, { flush: 'sync' })
watch([mdiBlock, (): number | undefined => panel.status?.offsets.wcs], () => { review.value = null }, { flush: 'sync' })
</script>

<template>
  <ModalDialog v-if="review !== null" key="review" :title="reviewTitle" size="medium" expanded fresh-gesture @close="review = null">
    <table class="changes">
      <thead><tr><th>{{ t('probe.workOffset.axis') }}</th><th>{{ t('probe.workOffset.current', { unit: unitName }) }}</th><th>{{ t('probe.workOffset.new', { unit: unitName }) }}</th></tr></thead>
      <tbody><tr v-for="(axis, index) in axes" :key="axis"><th class="mono">{{ axis }}</th><td class="mono">{{ review.before === null ? t('common.unavailable') : formatLength(review.before[index]!, unit) }}</td><td class="mono">{{ formatLength(review.target[index]!, unit) }}</td></tr></tbody>
    </table>
    <p>{{ t('probe.workOffset.replaces', { wcs: wcsName(review.wcs) }) }}</p>
    <p v-if="panel.status?.offsets.wcs === review.wcs" class="hint">{{ t('probe.workOffset.active', { wcs: wcsName(review.wcs) }) }}</p>
    <p v-if="reviewBlock" class="blocked" role="status">{{ reviewBlock }}</p>
    <template #footer><button class="btn" type="button" @click="review = null">{{ t('common.back') }}</button><button class="btn primary" type="button" :disabled="reviewBlock !== null" @click="confirm">{{ t('probe.workOffset.confirm', { wcs: wcsName(review.wcs) }) }}</button></template>
  </ModalDialog>
  <ModalDialog v-else key="edit" :title="t('probe.workOffset.title', { wcs: wcsName(wcs) })" size="medium" expanded @close="emit('close')">
    <p class="hint">{{ t('probe.workOffset.hint', { unit: unitName }) }}</p>
    <p v-if="original === null" class="hint">{{ t('probe.workOffset.unknown') }}</p>
    <p v-else-if="unconfirmed" class="hint unconfirmed" role="status">{{ t('probe.workOffset.unconfirmed') }}</p>
    <div class="coordinates">
      <label v-for="(axis, index) in axes" :key="axis" class="entry"><span class="mono">{{ axis }}</span><input v-model="values[index]" v-numeric :data-numeric-unit="unit" :aria-label="t('probe.workOffset.originLabel', { wcs: wcsName(wcs), axis })" class="field mono" type="text" inputmode="decimal" autocomplete="off" :placeholder="original === null ? t('common.unavailable') : ''" /></label>
    </div>
    <div class="offset-actions">
      <button class="btn" type="button" :disabled="block !== null" @click="prepare('clear')">{{ t('probe.workOffset.clear') }}</button>
      <span class="spacer"></span>
      <label class="copy-target"><span>{{ t('probe.workOffset.copyTo') }}</span><select v-model="destination" class="field mono" :aria-label="t('probe.workOffset.copyDestination')"><option v-for="(name, index) in wcsNames" :key="name" :value="index + 1" :disabled="index + 1 === wcs">{{ name }}</option></select></label>
      <button class="btn" type="button" :disabled="block !== null || parsed === null" @click="prepare('copy')">{{ t('probe.workOffset.copy') }}</button>
    </div>
    <p v-if="block" class="blocked" role="status">{{ block }}</p>
    <template #footer><button class="btn" type="button" @click="emit('close')">{{ t('common.cancel') }}</button><button class="btn primary" type="button" :disabled="block !== null || parsed === null" @click="prepare('set')">{{ t('probe.workOffset.review') }}</button></template>
  </ModalDialog>
</template>

<style scoped>
p { margin: 0; line-height: 1.45; }
.hint.unconfirmed { color: var(--warn); }
.coordinates { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 12px; }
.entry { display: flex; flex-direction: column; gap: 8px; font-size: 18px; }.entry input { width: 100%; min-width: 0; min-height: 48px; font-size: 20px; }
.offset-actions, .copy-target { display: flex; align-items: center; gap: 8px; }.offset-actions { padding-top: 12px; border-top: 1px solid var(--line); }.copy-target { font-size: 13px; }.copy-target select { min-height: 44px; }
.spacer { flex: 1; }.btn { min-height: 44px; }
.changes { width: 100%; border-collapse: collapse; }.changes th, .changes td { padding: 10px 8px; border-bottom: 1px solid var(--line); text-align: right; }.changes th:first-child { text-align: left; }.changes thead { color: var(--text-dim); font-size: 12px; }.changes td { font-size: 19px; }
</style>
