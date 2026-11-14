<script setup lang="ts">
import { computed, defineAsyncComponent, reactive, ref, watch } from 'vue'
import { panel } from '../machine'
import type { Tool } from '../protocol'
import { factoryToolDefaults, pointedTool, resolveToolModel, toolDimensionError, toolFamilies, toolIdentity,
  type ToolDimensions, type ToolFamily, type ToolModel } from '../tool-model'
import { persistToolDefaults, toolModels } from '../tool-models'
import { fromDisplay, parseNumber, toDisplay } from '../units'
import ModalDialog from './ModalDialog.vue'

const ToolModelCanvas = defineAsyncComponent(() => import('./ToolModelCanvas.vue'))
const props = defineProps<{ tool: Tool | null }>()
const emit = defineEmits<{ close: [] }>()
const defaultsMode = props.tool === null
const unit = panel.unit // Keep an open editor's unit stable.
const identity = props.tool === null ? '' : toolIdentity(props.tool)
const saved = props.tool === null ? undefined : toolModels.assignments.get(props.tool.number)
const family = ref<ToolFamily>(saved?.identity === identity ? saved.family : 'end-mill')
const custom = ref<boolean>(saved?.identity === identity && Object.keys(saved.overrides).length > 0)
const fields: { key: keyof ToolDimensions; label: string }[] = [
  { key: 'diameter', label: defaultsMode ? 'Reference diameter' : 'Cutting diameter' },
  { key: 'shankDiameter', label: 'Shank diameter' },
  { key: 'cuttingLength', label: 'Cutting length' },
  { key: 'overallLength', label: 'Overall length' },
  { key: 'stickout', label: 'Stick-out' },
  { key: 'tipAngle', label: 'Included tip angle' },
]
const form = reactive<Record<keyof ToolDimensions, string>>({ diameter: '', cuttingLength: '', shankDiameter: '', overallLength: '', stickout: '', tipAngle: '' })
const draftDefaults = reactive(factoryToolDefaults())
for (const { id } of toolFamilies) draftDefaults[id] = { ...toolModels.defaults[id] }
const baseline = computed(() => props.tool === null ? null : resolveToolModel(props.tool, { identity, family: family.value, overrides: {} }, draftDefaults))
function fill(d: ToolDimensions): void {
  for (const { key } of fields) form[key] = String(Number((key === 'tipAngle' ? d[key] : toDisplay(d[key], unit)).toFixed(6)))
}
function load(): void {
  fill(defaultsMode ? draftDefaults[family.value] : baseline.value?.dimensions ?? draftDefaults[family.value])
}
load()
if (custom.value && props.tool !== null && saved !== undefined) {
  const resolved = resolveToolModel(props.tool, saved, draftDefaults)
  if (resolved !== null) fill(resolved.dimensions)
}
const dimensions = computed((): ToolDimensions => {
  const d = { ...draftDefaults[family.value] }
  for (const { key } of fields) {
    const n = parseNumber(form[key]) ?? NaN
    d[key] = key === 'tipAngle' ? n : fromDisplay(n, unit)
  }
  // Never round or override the controller's diameter via the visual editor.
  if (!defaultsMode && baseline.value !== null) d.diameter = baseline.value.dimensions.diameter
  return d
})
const model = computed((): ToolModel => ({ family: family.value,
  dimensions: !defaultsMode && !custom.value && baseline.value !== null ? baseline.value.dimensions : dimensions.value,
  nominalDiameter: baseline.value?.nominalDiameter ?? true, customized: custom.value }))
const error = computed(() => toolDimensionError(family.value, model.value.dimensions))
const changedTool = computed(() => props.tool !== null && !panel.toolTable?.tools.some((t) => toolIdentity(t) === identity))
watch(family, (_current, previous) => {
  // Preserve valid edits while browsing family defaults; invalid drafts are not stored.
  if (defaultsMode) {
    const d = { ...dimensions.value }
    if (toolDimensionError(previous, d) === null) draftDefaults[previous] = d
  }
  custom.value = false
  load()
})
watch(custom, (value) => { if (!value) load() })
function reset(): void { fill(factoryToolDefaults()[family.value]) }
function save(): void {
  if (error.value !== null || changedTool.value) return
  if (defaultsMode) {
    draftDefaults[family.value] = { ...dimensions.value }
    const copy = factoryToolDefaults()
    for (const { id } of toolFamilies) copy[id] = { ...draftDefaults[id] }
    persistToolDefaults(copy)
  } else {
    const { cuttingLength, shankDiameter, overallLength, stickout, tipAngle } = model.value.dimensions
    const overrides = { cuttingLength, shankDiameter, overallLength, stickout, tipAngle }
    toolModels.assignments.set(props.tool.number, { identity, family: family.value, overrides: custom.value ? overrides : {} })
  }
  emit('close')
}
function clearAssignment(): void {
  if (props.tool !== null) toolModels.assignments.delete(props.tool.number)
  emit('close')
}
</script>

<template>
  <ModalDialog :title="defaultsMode ? 'Tool model defaults' : `T${tool?.number} · preview model`" size="large" contained expanded @close="emit('close')">
    <div class="editor">
      <div class="settings">
        <label class="entry"><span class="label">Tool family</span>
          <select v-model="family" class="field"><option v-for="f in toolFamilies" :key="f.id" :value="f.id">{{ f.name }}</option></select>
        </label>
        <p v-if="defaultsMode" class="hint explanation">Lengths scale from the reference diameter to each tool's table diameter. Tip angle stays fixed.</p>
        <template v-else>
          <label class="check"><input v-model="custom" type="checkbox" />Override dimensions for this tool</label>
          <p class="hint explanation">{{ model.nominalDiameter ? 'No table diameter: using the family’s nominal diameter.' : 'Cutting diameter comes from the tool table.' }} {{ custom ? 'Overrides are absolute dimensions.' : 'Other lengths use the family’s scaled defaults.' }}</p>
        </template>
        <div class="dimensions">
          <label v-for="field in fields.filter((f) => f.key !== 'tipAngle' || pointedTool(family))" :key="field.key" class="entry">
            <span class="label">{{ field.label }} <span class="hint">{{ field.key === 'tipAngle' ? '°' : unit }}</span></span>
            <input v-model="form[field.key]" class="field mono" type="text" inputmode="decimal" :disabled="!defaultsMode && (!custom || field.key === 'diameter')" />
          </label>
        </div>
        <p v-if="error" class="blocked">{{ error }}</p>
        <p v-if="changedTool" class="blocked">The tool table changed. Close this editor and reopen the current tool.</p>
        <p class="hint explanation">Stick-out runs from the tip to the holder face. These dimensions are independent of the LinuxCNC Z offset.</p>
        <p class="scope hint">{{ defaultsMode ? 'Family defaults are saved in this browser.' : 'Tool assignments last for this page session; changed or removed tools lose their assignment.' }} Simplified envelopes; flutes, inserts and holder geometry are not represented.</p>
      </div>
      <div class="preview">
        <ToolModelCanvas v-if="error === null" :model="model" />
        <div v-else class="invalid hint">Enter valid dimensions to preview the tool.</div>
      </div>
    </div>
    <template #footer>
      <button v-if="defaultsMode" class="btn quiet" type="button" @click="reset">Reset this family</button>
      <button v-else class="btn quiet" type="button" @click="clearAssignment">Clear model</button>
      <span class="spacer"></span>
      <button class="btn" type="button" @click="emit('close')">Cancel</button>
      <button class="btn primary" type="button" :disabled="error !== null || changedTool" @click="save">{{ defaultsMode ? 'Save defaults' : 'Save preview model' }}</button>
    </template>
  </ModalDialog>
</template>

<style scoped>
.editor { display: grid; grid-template-columns: minmax(0, 1fr) minmax(0, 1fr); gap: 24px; flex: 1; min-height: 0; }
.settings { display: flex; flex-direction: column; gap: 12px; overflow: auto; padding-right: 8px; }.entry { display: flex; flex-direction: column; gap: 5px; }
.dimensions { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 12px; }.dimensions .field { width: 100%; min-width: 0; }.dimensions .label { min-height: 30px; }
.explanation, .scope, .blocked { margin: 0; line-height: 1.5; }.check { display: flex; align-items: center; gap: 8px; font-size: 12px; }
.scope { padding-top: 12px; border-top: 1px solid var(--line); }.spacer { flex: 1; }
.preview { min-width: 0; min-height: 0; }.invalid { display: grid; place-items: center; height: 100%; text-align: center; padding: 20px; background: var(--bg-sunken); border-radius: 8px; }
@media (max-height: 850px) { .editor { gap: 16px; }.settings { gap: 9px; }.dimensions { gap: 8px 12px; } }
@media (max-width: 900px) { .dimensions { grid-template-columns: 1fr 1fr; } }
@media (max-width: 700px) { .editor { grid-template-columns: minmax(0, 1.2fr) minmax(0, 1fr); }.dimensions { grid-template-columns: 1fr; } }
</style>
