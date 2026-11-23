<script setup lang="ts">
import { computed, ref, useTemplateRef, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import sampleUrl from '../../../samples/upstream/instant-machinist/instant-machinist/parts/sm-101/part.step?url'
import { live, panel } from '../machine'
import { cancelImport, importPart, part, partBlock, partNote, placePart, removePart, sampleNote } from '../step'
import { parseNumericExpression } from '../numeric-expression'
import { formatLength, wcsName } from '../units'
import type { Vec3 } from '../protocol'
import ModalDialog from './ModalDialog.vue'
import { vNumeric } from '../numeric-input'
import { vTooltip } from '../tooltip'
import { translateMessage } from '../i18n/messages'

const { t } = useI18n()

const open = ref(false)
const chooser = useTemplateRef<HTMLInputElement>('chooser')
const translation = ref<[string, string, string]>(['0', '0', '0'])
const rotation = ref<[string, string, string]>(['0', '0', '0'])
const formError = ref('')
const sampleLoading = ref(false)
const axes = ['X', 'Y', 'Z'] as const
const available = computed(() => live.value && Boolean(panel.status?.program.file))
const bounds = computed(() => part.attachment?.geometry.bounds)
const summary = computed(() => part.loading ? t('program.partModel.importing') : part.attachment === null ? t('program.partModel.attach') : part.attachment.name)
const note = computed(() => part.attachment === null ? '' : partNote(part.attachment.note))
const detail = computed(() => part.attachment
  ? partBlock.value || (part.placed ? t('program.partModel.placedByOperator') : note.value)
  : translateMessage(part.error))
watch([open, (): unknown => part.attachment], () => {
  translation.value = part.placement.translation.map(String) as [string, string, string]
  rotation.value = part.placement.rotation.map(String) as [string, string, string]
  formError.value = ''
})

function selected(event: Event): void {
  const input = event.target as HTMLInputElement
  const file = input.files?.[0]
  input.value = ''
  if (file !== undefined) void importPart(file)
}

async function sample(): Promise<void> {
  sampleLoading.value = true
  const programFile = panel.status?.program.file
  const revision = panel.status?.program.revision
  try {
    const response = await fetch(sampleUrl)
    if (!response.ok) throw new Error('Could not load the sample model') // i18n-ignore
    const bytes = await response.arrayBuffer()
    if (panel.status?.program.file !== programFile || panel.status?.program.revision !== revision) return
    await importPart(new File([bytes], 'SM-101.step'), sampleNote)
  } catch { part.error = t('program.partModel.sampleFailed') }
  finally { sampleLoading.value = false }
}

function apply(): void {
  const offsets = translation.value.map((text) => parseNumericExpression(text, 'mm'))
  const r = rotation.value.map((text) => parseNumericExpression(text, '°'))
  if (offsets.some((n) => n === null || !Number.isFinite(n) || Math.abs(n) > 1e6) || r.some((n) => n === null || !Number.isFinite(n) || Math.abs(n) > 360)) {
    formError.value = t('program.partModel.badPlacement'); return
  }
  if (!placePart({ translation: offsets as Vec3, rotation: r as Vec3 })) { formError.value = t('program.partModel.placementUnavailable'); return }
  formError.value = ''
  open.value = false
}
</script>

<template>
  <div class="model-summary">
    <button
      v-tooltip="detail ? summary + ' · ' + detail : summary" class="btn attach" type="button"
      :class="{ on: part.attachment && part.visible }" :aria-label="'STEP · ' + summary" aria-haspopup="dialog"
      @click="open = true"
    >
      STEP
    </button>
    <span class="model-status" role="status">{{ summary }}{{ detail ? ' · ' + detail : '' }}</span>
  </div>
  <ModalDialog v-if="open" :title="t('program.partModel.title')" size="medium" @close="open = false">
    <div class="load-row">
      <input ref="chooser" class="file-input" type="file" accept=".step,.stp" :aria-label="t('program.partModel.chooseLabel')" @change="selected" />
      <button class="btn" type="button" :disabled="!available || part.loading" @click="chooser?.click()">{{ t('program.partModel.choose') }}</button>
      <button v-if="panel.simulated" class="btn quiet" type="button" :disabled="!available || part.loading || sampleLoading" @click="sample">{{ t('program.partModel.loadSample') }}</button>
      <button v-if="part.loading" class="btn" type="button" @click="cancelImport">{{ t('program.partModel.cancelImport') }}</button>
    </div>
    <p v-if="!available" class="hint">{{ t('program.partModel.loadProgram') }}</p>
    <p v-if="part.loading" class="hint" role="status">{{ t('program.partModel.reading') }}</p>
    <p v-if="part.error" class="error" role="alert">{{ translateMessage(part.error) }}</p>
    <template v-if="part.attachment">
      <div class="identity"><strong>{{ part.attachment.name }}</strong><span>{{ t('program.partModel.parts', part.attachment.geometry.meshes.length) }} · {{ t('program.partModel.triangles', { count: String(part.attachment.geometry.triangles) }, part.attachment.geometry.triangles) }}</span></div>
      <p v-if="note" class="notice">{{ note }}</p>
      <div class="display-row"><label><input v-model="part.visible" type="checkbox" /> {{ t('program.partModel.showPart') }}</label><label class="opacity">{{ t('program.partModel.opacity') }} <input v-model.number="part.opacity" type="range" min="0.1" max="1" step="0.05" /><span class="opacity-value mono">{{ Math.round(part.opacity * 100) }}%</span></label><button class="btn quiet" type="button" @click="removePart">{{ t('common.remove') }}</button></div>
      <div class="placement-head"><strong>{{ t('program.partModel.placementIn', { wcs: wcsName(panel.status?.offsets.wcs ?? 1) }) }}</strong><small>{{ t('program.partModel.displayOnly') }}</small></div>
      <div class="placement-grid"><span></span><span v-for="axis in axes" :key="axis">{{ axis }}</span><label>{{ t('program.partModel.offset') }}</label><input v-for="(axis, i) in axes" :key="`t${axis}`" v-model="translation[i]" v-numeric data-numeric-unit="mm" type="text" inputmode="decimal" :aria-label="t('program.partModel.offsetLabel', { axis })" /><label>{{ t('program.partModel.rotation') }}</label><input v-for="(axis, i) in axes" :key="`r${axis}`" v-model="rotation[i]" v-numeric data-numeric-unit="°" type="text" inputmode="decimal" :aria-label="t('program.partModel.rotationLabel', { axis })" /></div>
      <p class="hint">{{ t('program.partModel.rotateHint') }}</p>
      <p v-if="bounds" class="hint">{{ t('program.partModel.cadSize', { size: axes.map((_, i) => formatLength((bounds?.max[i] ?? 0) - (bounds?.min[i] ?? 0), 'mm')).join(' × ') }) }}</p>
      <p v-if="partBlock" class="notice">{{ partBlock }}</p>
      <p v-if="formError" class="error" role="alert">{{ formError }}</p>
    </template>
    <p v-else class="hint">{{ t('program.partModel.intro') }}</p>
    <template #footer><button class="btn" type="button" @click="open = false">{{ t('common.close') }}</button><button v-if="part.attachment" class="btn primary" type="button" :disabled="!available || part.loading" @click="apply">{{ t('program.partModel.applyPlacement') }}</button></template>
  </ModalDialog>
</template>

<style scoped>
.model-summary { flex: none; }
.attach { padding-inline: 9px; }
.model-status { position: absolute; width: 1px; height: 1px; overflow: hidden; clip-path: inset(50%); white-space: nowrap; }
small { font-size: var(--type-sm); color: var(--text-dim); }
.file-input { position: absolute; width: 1px; height: 1px; opacity: 0; overflow: hidden; }
.load-row, .display-row { display: flex; align-items: center; gap: 12px; flex-wrap: wrap; }
.identity { display: flex; flex-direction: column; gap: 4px; font-size: 13px; }.identity span, .hint { color: var(--text-dim); font-size: 12px; }.hint, .notice, .error { margin: 0; line-height: 1.5; }.notice { color: var(--warn); font-size: 12px; }.error { color: var(--stop); font-size: 12px; }
.display-row { font-size: 12px; }.display-row label { display: flex; align-items: center; gap: 6px; }.opacity { flex: 1; }.opacity input { min-width: 70px; width: 100px; flex: 1; }
.opacity-value { flex: none; width: 4ch; min-width: 4ch; text-align: right; }
.placement-head { display: flex; align-items: baseline; justify-content: space-between; gap: 12px; border-top: 1px solid var(--line); padding-top: 12px; font-size: 13px; }
.placement-grid { display: grid; grid-template-columns: 100px repeat(3, minmax(0, 1fr)); align-items: center; gap: 6px 10px; font-size: 12px; }.placement-grid > span { color: var(--text-dim); }.placement-grid input { min-width: 0; width: 100%; padding: 8px; border: 0; border-radius: var(--control-radius); background: var(--field); color: var(--text); }
</style>
