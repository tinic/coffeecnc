<script setup lang="ts">
import { computed, ref, watch } from 'vue'
import { mdiBlock, panel, send } from '../machine'
import { vNumeric } from '../numeric-input'
import type { Vec3 } from '../protocol'
import { formatLength, parseLength, toDisplay, wcsName, wcsNames } from '../units'
import { workOffsetRows } from '../work-offsets'
import ModalDialog from './ModalDialog.vue'

const props = defineProps<{ wcs: number }>()
const emit = defineEmits<{ close: [] }>()
const unit = panel.unit
const hello = panel.hello
const axes = ['X', 'Y', 'Z'] as const
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
const block = computed(() => mdiBlock.value ?? (panel.status?.link !== 'live' || panel.link !== 'live'
  ? 'Wait for current machine status.' : panel.hello !== hello ? 'The connection changed. Reopen this editor.'
    : !equal(current(props.wcs), original) ? 'These origins changed. Reopen this editor to use their current values.' : null))
interface Review { kind: 'Set' | 'Copy' | 'Clear'; wcs: number; before: Vec3 | null; target: Vec3 }
const review = ref<Review | null>(null)
const reviewBlock = computed(() => block.value ?? (review.value !== null && !equal(current(review.value.wcs), review.value.before)
  ? 'The destination origins changed. Go back and review them again.' : null))
const reviewTitle = computed(() => review.value === null ? '' :
  `${review.value.kind === 'Copy' ? 'Copy XYZ to' : review.value.kind === 'Clear' ? 'Clear XYZ in' : 'Set XYZ in'} ${wcsName(review.value.wcs)}?`)
function prepare(kind: Review['kind']): void {
  const target: Vec3 | null = kind === 'Clear' ? [0, 0, 0] : parsed.value
  if (block.value !== null || target === null) return
  const wcs = kind === 'Copy' ? destination.value : props.wcs
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
      <thead><tr><th>Axis</th><th>Current {{ unit === 'inch' ? 'in' : 'mm' }}</th><th>New {{ unit === 'inch' ? 'in' : 'mm' }}</th></tr></thead>
      <tbody><tr v-for="(axis, index) in axes" :key="axis"><th class="mono">{{ axis }}</th><td class="mono">{{ review.before === null ? 'Unavailable' : formatLength(review.before[index]!, unit) }}</td><td class="mono">{{ formatLength(review.target[index]!, unit) }}</td></tr></tbody>
    </table>
    <p>Replaces {{ wcsName(review.wcs) }} XYZ origins in machine coordinates. The machine does not move. XY rotation and G92/G52 shifts stay unchanged.</p>
    <p v-if="panel.status?.offsets.wcs === review.wcs" class="hint">{{ wcsName(review.wcs) }} is active; the work readout changes immediately.</p>
    <p v-if="reviewBlock" class="blocked" role="status">{{ reviewBlock }}</p>
    <template #footer><button class="btn" type="button" @click="review = null">Back</button><button class="btn primary" type="button" :disabled="reviewBlock !== null" @click="confirm">Confirm {{ wcsName(review.wcs) }} XYZ</button></template>
  </ModalDialog>
  <ModalDialog v-else key="edit" :title="`${wcsName(wcs)} work origins`" size="medium" expanded @close="emit('close')">
    <p class="hint">XYZ origins in machine coordinates · {{ unit === 'inch' ? 'in' : 'mm' }}</p>
    <p v-if="original === null" class="hint">Current origins are unavailable. Enter all three values to replace them, or close and select this system to read it first.</p>
    <div class="coordinates">
      <label v-for="(axis, index) in axes" :key="axis" class="entry"><span class="mono">{{ axis }}</span><input v-model="values[index]" v-numeric :data-numeric-unit="unit" :aria-label="`${wcsName(wcs)} ${axis} origin`" class="field mono" type="text" inputmode="decimal" autocomplete="off" :placeholder="original === null ? 'Unavailable' : ''" /></label>
    </div>
    <div class="offset-actions">
      <button class="btn" type="button" :disabled="block !== null" @click="prepare('Clear')">Clear XYZ…</button>
      <span class="spacer"></span>
      <label class="copy-target"><span>Copy to</span><select v-model="destination" class="field mono" aria-label="Copy destination"><option v-for="(name, index) in wcsNames" :key="name" :value="index + 1" :disabled="index + 1 === wcs">{{ name }}</option></select></label>
      <button class="btn" type="button" :disabled="block !== null || parsed === null" @click="prepare('Copy')">Copy XYZ…</button>
    </div>
    <p v-if="block" class="blocked" role="status">{{ block }}</p>
    <template #footer><button class="btn" type="button" @click="emit('close')">Cancel</button><button class="btn primary" type="button" :disabled="block !== null || parsed === null" @click="prepare('Set')">Review changes</button></template>
  </ModalDialog>
</template>

<style scoped>
p { margin: 0; line-height: 1.45; }
.coordinates { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 12px; }
.entry { display: flex; flex-direction: column; gap: 8px; font-size: 18px; }.entry input { width: 100%; min-width: 0; min-height: 48px; font-size: 20px; }
.offset-actions, .copy-target { display: flex; align-items: center; gap: 8px; }.offset-actions { padding-top: 12px; border-top: 1px solid var(--line); }.copy-target { font-size: 13px; }.copy-target select { min-height: 44px; }
.spacer { flex: 1; }.btn { min-height: 44px; }
.changes { width: 100%; border-collapse: collapse; }.changes th, .changes td { padding: 10px 8px; border-bottom: 1px solid var(--line); text-align: right; }.changes th:first-child { text-align: left; }.changes thead { color: var(--text-dim); font-size: 12px; }.changes td { font-size: 19px; }
</style>
