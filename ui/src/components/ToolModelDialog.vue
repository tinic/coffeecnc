<script setup lang="ts">
import { computed, defineAsyncComponent, onBeforeUnmount, reactive, ref, watch } from 'vue'
import { panel } from '../machine'
import type { Tool } from '../protocol'
import { factoryToolDefaults, pointedTool, resolveToolModel, toolDimensionError, toolFamilies, toolIdentity,
  type ToolDimensions, type ToolFamily, type ToolModel } from '../tool-model'
import { toolModelSettings, toolModels } from '../tool-models'
import { fromDisplay, parseNumber, toDisplay } from '../units'
import ModalDialog from './ModalDialog.vue'

const ToolModelCanvas = defineAsyncComponent(() => import('./ToolModelCanvas.vue'))
const props = defineProps<{ tool: Tool | null }>()
const emit = defineEmits<{ close: [] }>()
const defaultsMode = props.tool === null
const editContext = toolModelSettings.context()
const saving = ref(false)
const saveError = ref('')
let open = true
onBeforeUnmount(() => { open = false })
const saveBlock = computed(() => saving.value ? null : toolModelSettings.block(editContext) ??
  (props.tool !== null && !panel.simulated && panel.link !== 'live' ? 'A current matching tool table is required. Wait for the machine connection.' : null))
const scope = computed(() => {
  switch (toolModels.storage) {
    case 'browser': return defaultsMode ? 'Family defaults are saved in this browser.' : 'Tool assignments last for this page session.'
    case 'available': return `Save keeps these models for machine ${panel.hello?.machine_id ?? ''}. Restored assignments require a matching tool-table row.`
    case 'session_only': return 'Changes last until this page closes.'
    case 'loading': return 'Waiting for this machine’s tool model settings.'
    case 'unavailable': return 'Saved tool models are unavailable.'
  }
})
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
async function save(): Promise<void> {
  if (error.value !== null || changedTool.value || saving.value || saveBlock.value !== null) return
  saving.value = true
  saveError.value = ''
  let result
  if (defaultsMode) {
    draftDefaults[family.value] = { ...dimensions.value }
    const copy = factoryToolDefaults()
    for (const { id } of toolFamilies) copy[id] = { ...draftDefaults[id] }
    result = await toolModelSettings.saveDefaults(copy, editContext)
  } else {
    const { cuttingLength, shankDiameter, overallLength, stickout, tipAngle } = model.value.dimensions
    const overrides = { cuttingLength, shankDiameter, overallLength, stickout, tipAngle }
    result = await toolModelSettings.saveAssignment(props.tool, family.value, custom.value ? overrides : {}, editContext)
  }
  saving.value = false
  if (result.saved) { if (open) emit('close') }
  else saveError.value = result.reason
}
async function clearAssignment(): Promise<void> {
  if (props.tool === null || saving.value || changedTool.value || saveBlock.value !== null) return
  saving.value = true
  saveError.value = ''
  const result = await toolModelSettings.saveAssignment(props.tool, null, {}, editContext)
  saving.value = false
  if (result.saved) { if (open) emit('close') }
  else saveError.value = result.reason
}
</script>

<template>
  <ModalDialog :title="defaultsMode ? 'Tool model defaults' : `T${tool?.number} · preview model`" size="large" contained expanded @close="emit('close')">
    <div class="editor">
      <div class="settings">
        <label class="entry"><span class="label">Tool family</span>
          <select v-model="family" class="field" :disabled="saving"><option v-for="f in toolFamilies" :key="f.id" :value="f.id">{{ f.name }}</option></select>
        </label>
        <p v-if="defaultsMode" class="hint explanation">Lengths scale from the reference diameter to each tool's table diameter. Tip angle stays fixed.</p>
        <template v-else>
          <label class="check"><input v-model="custom" type="checkbox" :disabled="saving" />Override dimensions for this tool</label>
          <p class="hint explanation">{{ model.nominalDiameter ? 'No table diameter: using the family’s nominal diameter.' : 'Cutting diameter comes from the tool table.' }} {{ custom ? 'Overrides are absolute dimensions.' : 'Other lengths use the family’s scaled defaults.' }}</p>
        </template>
        <div class="dimensions">
          <label v-for="field in fields.filter((f) => f.key !== 'tipAngle' || pointedTool(family))" :key="field.key" class="entry">
            <span class="label">{{ field.label }} <span class="hint">{{ field.key === 'tipAngle' ? '°' : unit }}</span></span>
            <input v-model="form[field.key]" class="field mono" type="text" inputmode="decimal" :disabled="saving || !defaultsMode && (!custom || field.key === 'diameter')" />
          </label>
        </div>
        <p class="hint explanation">Stick-out runs from the tip to the holder face. These dimensions are independent of the LinuxCNC Z offset.</p>
        <p class="scope hint">{{ scope }} Simplified envelopes; flutes, inserts and holder geometry are not represented.</p>
      </div>
      <div class="preview">
        <ToolModelCanvas v-if="error === null" :model="model" />
        <div v-else class="invalid hint">Enter valid dimensions to preview the tool.</div>
      </div>
    </div>
    <p v-if="saveError || saveBlock || changedTool || error || saving" class="feedback" :class="saving ? 'hint' : 'blocked'" role="status">
      {{ saveError || saveBlock || (changedTool ? 'The tool table changed. Close this editor and reopen the current tool.' : error) || 'Saving… Closing this editor does not cancel the save.' }}
    </p>
    <template #footer>
      <button v-if="defaultsMode" class="btn quiet" type="button" :disabled="saving" @click="reset">Reset this family</button>
      <button v-else class="btn quiet" type="button" :disabled="saving || changedTool || saveBlock !== null" @click="clearAssignment">Clear model</button>
      <span class="spacer"></span>
      <button class="btn" type="button" @click="emit('close')">{{ saving ? 'Close' : 'Cancel' }}</button>
      <button class="btn primary" type="button" :disabled="saving || error !== null || changedTool || saveBlock !== null" @click="save">{{ saving ? 'Saving…' : defaultsMode ? 'Save defaults' : 'Save preview model' }}</button>
    </template>
  </ModalDialog>
</template>

<style scoped>
.editor { display: grid; grid-template-columns: minmax(0, 1fr) minmax(0, 1fr); gap: 24px; flex: 1; min-height: 0; }
.settings { display: flex; flex-direction: column; gap: 12px; overflow: auto; padding-right: 8px; }.entry { display: flex; flex-direction: column; gap: 5px; }
.dimensions { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 12px; }.dimensions .field { width: 100%; min-width: 0; }.dimensions .label { min-height: 30px; }
.explanation, .scope, .blocked { margin: 0; line-height: 1.5; }.check { display: flex; align-items: center; gap: 8px; font-size: 12px; }
.feedback { flex: none; margin: 0; line-height: 1.5; }
.scope { padding-top: 12px; border-top: 1px solid var(--line); }.spacer { flex: 1; }
.preview { min-width: 0; min-height: 0; }.invalid { display: grid; place-items: center; height: 100%; text-align: center; padding: 20px; background: var(--bg-sunken); border-radius: 8px; }
@media (max-height: 850px) { .editor { gap: 16px; }.settings { gap: 9px; }.dimensions { gap: 8px 12px; } }
@media (max-width: 900px) { .dimensions { grid-template-columns: 1fr 1fr; } }
@media (max-width: 700px) { .editor { grid-template-columns: minmax(0, 1.2fr) minmax(0, 1fr); }.dimensions { grid-template-columns: 1fr; } }
</style>
