<script setup lang="ts">
import { computed, ref, useTemplateRef, watch } from 'vue'
import sampleUrl from '../../../samples/upstream/instant-machinist/instant-machinist/parts/sm-101/part.step?url'
import { live, panel } from '../machine'
import { cancelImport, importPart, part, partBlock, placePart, removePart } from '../step'
import { formatLength, parseNumber, wcsName } from '../units'
import type { Vec3 } from '../protocol'
import ModalDialog from './ModalDialog.vue'
import { vNumeric } from '../numeric-input'

const open = ref(false)
const chooser = useTemplateRef<HTMLInputElement>('chooser')
const translation = ref<[string, string, string]>(['0', '0', '0'])
const rotation = ref<[string, string, string]>(['0', '0', '0'])
const formError = ref('')
const sampleLoading = ref(false)
const axes = ['X', 'Y', 'Z'] as const
const available = computed(() => live.value && Boolean(panel.status?.program.file))
const bounds = computed(() => part.attachment?.geometry.bounds)
const summary = computed(() => part.loading ? 'Importing STEP…' : part.attachment === null ? 'Attach STEP model' : part.attachment.name)
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
    if (!response.ok) throw new Error('Could not load the sample model')
    const bytes = await response.arrayBuffer()
    if (panel.status?.program.file !== programFile || panel.status?.program.revision !== revision) return
    await importPart(new File([bytes], 'SM-101.step'), 'Sample geometry; not a matched model for this program')
  } catch { part.error = 'Could not load the sample model' }
  finally { sampleLoading.value = false }
}

function apply(): void {
  const t = translation.value.map(parseNumber)
  const r = rotation.value.map(parseNumber)
  if (t.some((n) => n === null || !Number.isFinite(n) || Math.abs(n) > 1e6) || r.some((n) => n === null || !Number.isFinite(n) || Math.abs(n) > 360)) {
    formError.value = 'Enter finite offsets within ±1,000,000 mm and angles within ±360°'; return
  }
  if (!placePart({ translation: t as Vec3, rotation: r as Vec3 })) { formError.value = 'Current program and work origin are unavailable'; return }
  formError.value = ''
  open.value = false
}
</script>

<template>
  <div class="model-summary">
    <button class="attach" type="button" @click="open = true"><span class="model-mark">◇</span><span>{{ summary }}</span><span>›</span></button>
    <small v-if="part.attachment">{{ partBlock || (part.placed ? 'Placement set by operator' : 'Placement unconfirmed') }}</small>
    <small v-else-if="part.error" class="error">{{ part.error }}</small>
    <small v-else>Optional part geometry</small>
  </div>
  <ModalDialog v-if="open" title="Part model · STEP" size="medium" @close="open = false">
    <div class="load-row">
      <input ref="chooser" class="file-input" type="file" accept=".step,.stp" aria-label="Choose STEP file" @change="selected" />
      <button class="btn" type="button" :disabled="!available || part.loading" @click="chooser?.click()">Choose STEP file…</button>
      <button v-if="panel.simulated" class="btn quiet" type="button" :disabled="!available || part.loading || sampleLoading" @click="sample">Load SM-101 sample</button>
      <button v-if="part.loading" class="btn" type="button" @click="cancelImport">Cancel import</button>
    </div>
    <p v-if="!available" class="hint">Load a program to attach its part model.</p>
    <p v-if="part.loading" class="hint" role="status">Reading and tessellating STEP…</p>
    <p v-if="part.error" class="error" role="alert">{{ part.error }}</p>
    <template v-if="part.attachment">
      <div class="identity"><strong>{{ part.attachment.name }}</strong><span>{{ part.attachment.geometry.meshes.length }} parts · {{ part.attachment.geometry.triangles.toLocaleString() }} triangles</span></div>
      <p v-if="part.attachment.note" class="notice">{{ part.attachment.note }}</p>
      <div class="display-row"><label><input v-model="part.visible" type="checkbox" /> Show part</label><label class="opacity">Opacity <input v-model.number="part.opacity" type="range" min="0.1" max="1" step="0.05" />{{ Math.round(part.opacity * 100) }}%</label><button class="btn quiet" type="button" @click="removePart">Remove</button></div>
      <div class="placement-head"><strong>Placement in {{ wcsName(panel.status?.offsets.wcs ?? 1) }}</strong><small>Display only · does not change work offsets</small></div>
      <div class="placement-grid"><span></span><span v-for="axis in axes" :key="axis">{{ axis }}</span><label>Offset (mm)</label><input v-for="(axis, i) in axes" :key="`t${axis}`" v-model="translation[i]" v-numeric type="text" inputmode="decimal" :aria-label="`Model ${axis} offset in millimeters`" /><label>Rotation (°)</label><input v-for="(axis, i) in axes" :key="`r${axis}`" v-model="rotation[i]" v-numeric type="text" inputmode="decimal" :aria-label="`Model ${axis} rotation in degrees`" /></div>
      <p class="hint">Rotate about the CAD origin in X → Y → Z order, then translate. Zero offsets place the CAD origin at the active work origin.</p>
      <p v-if="bounds" class="hint">CAD size: {{ axes.map((_, i) => formatLength((bounds?.max[i] ?? 0) - (bounds?.min[i] ?? 0), 'mm')).join(' × ') }} mm. Model and placement stay in mm.</p>
      <p v-if="partBlock" class="notice">{{ partBlock }}</p>
      <p v-if="formError" class="error" role="alert">{{ formError }}</p>
    </template>
    <p v-else class="hint">STEP files up to 25 MB. The model is a visual reference, separate from stock or material-removal simulation.</p>
    <template #footer><button class="btn" type="button" @click="open = false">Close</button><button v-if="part.attachment" class="btn primary" type="button" :disabled="!available || part.loading" @click="apply">Apply placement</button></template>
  </ModalDialog>
</template>

<style scoped>
.model-summary { flex: none; border-top: 1px solid var(--line); padding: 8px 20px 10px; }
.attach { width: 100%; display: flex; align-items: center; gap: 8px; padding: 0; min-height: 30px; border: 0; background: none; text-align: left; cursor: pointer; font-size: 12px; }.attach span:nth-child(2) { flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }.model-mark { color: var(--accent); font-size: 22px; }
small { font-size: 10px; color: var(--text-dim); }.model-summary small { display: block; margin-top: 4px; }
.file-input { position: absolute; width: 1px; height: 1px; opacity: 0; overflow: hidden; }
.load-row, .display-row { display: flex; align-items: center; gap: 12px; flex-wrap: wrap; }
.identity { display: flex; flex-direction: column; gap: 4px; font-size: 13px; }.identity span, .hint { color: var(--text-dim); font-size: 12px; }.hint, .notice, .error { margin: 0; line-height: 1.5; }.notice { color: var(--warn); font-size: 12px; }.error { color: var(--stop); font-size: 12px; }
.display-row { font-size: 12px; }.display-row label { display: flex; align-items: center; gap: 6px; }.opacity { flex: 1; }.opacity input { min-width: 70px; width: 100px; flex: 1; }
.placement-head { display: flex; align-items: baseline; justify-content: space-between; gap: 12px; border-top: 1px solid var(--line); padding-top: 12px; font-size: 13px; }
.placement-grid { display: grid; grid-template-columns: 100px repeat(3, minmax(0, 1fr)); align-items: center; gap: 6px 10px; font-size: 12px; }.placement-grid > span { color: var(--text-dim); }.placement-grid input { min-width: 0; width: 100%; padding: 8px; border: 1px solid var(--line); border-radius: 6px; background: var(--control); color: var(--text); }
</style>
