<script setup lang="ts">
import { computed, defineAsyncComponent, onBeforeUnmount, reactive, ref, shallowRef } from 'vue'
import { linkBlock, panel, send } from '../machine'
import { vNumeric } from '../numeric-input'
import type { Tool } from '../protocol'
import { sameTool, saveTool } from '../save-tool'
import { pointedTool, resolveToolModel, toolDimensionError, toolFamilies, toolIdentity,
  type ToolDimensions, type ToolFamily, type ToolModel } from '../tool-model'
import { toolModelSettings, toolModels } from '../tool-models'
import { parseLength, parseNumber, toDisplay } from '../units'
import ModalDialog from './ModalDialog.vue'

const ToolModelCanvas = defineAsyncComponent(() => import('./ToolModelCanvas.vue'))
const props = defineProps<{ tool: Tool | null }>()
const emit = defineEmits<{ close: [] }>()
const unit = panel.unit
const hello = panel.hello
const original = shallowRef<Tool | null>(props.tool === null ? null : { ...props.tool })
const modelContext = toolModelSettings.context()
let open = true
const pendingSave = new AbortController()
onBeforeUnmount(() => { open = false; pendingSave.abort() })
function isOpen(): boolean { return open }
const saving = ref(false)
const feedback = ref('')
const deleting = ref(false)
function text(value: number): string { return String(Number(toDisplay(value, unit).toFixed(6))) }
const form = reactive({ number: String(props.tool?.number ?? (Math.max(0, ...(panel.toolTable?.tools.map((tool) => tool.number) ?? [])) + 1)),
  length: text(props.tool?.length ?? 0), diameter: text(props.tool?.diameter ?? 0), comment: props.tool?.comment ?? '' })
// Keep unedited machine dimensions exact despite the shorter displayed text.
const initial = { length: form.length, diameter: form.diameter }
const parsed = computed((): Tool | null => {
  const number = parseNumber(form.number)
  const length = original.value !== null && form.length === initial.length ? original.value.length : parseLength(form.length, unit)
  const diameter = original.value !== null && form.diameter === initial.diameter ? original.value.diameter : parseLength(form.diameter, unit)
  if (number === null || !Number.isInteger(number) || number < 1 || number > 2147483647 || length === null || diameter === null || diameter < 0) return null
  return { number, pocket: original.value?.pocket ?? number, length, diameter, comment: form.comment.trim() }
})
const current = computed(() => panel.toolTable?.tools.find((tool) => tool.number === original.value?.number))
const block = computed(() => linkBlock.value ?? (panel.hello !== hello ? 'The machine connection changed. Reopen this editor.'
  : panel.status?.interp !== 'idle' ? 'Stop execution before editing the tool.'
    : !saving.value && original.value !== null && !sameTool(current.value, original.value) ? 'The tool table changed. Reopen this editor.'
      : null))
const saved = props.tool === null ? undefined : toolModels.assignments.get(props.tool.number)
const savedModel = props.tool === null ? null : resolveToolModel(props.tool, saved, toolModels.defaults)
const family = ref<ToolFamily | 'none'>(savedModel?.family ?? ((props.tool?.diameter ?? 0) > 0 ? 'end-mill' : 'none'))
const fields: { key: Exclude<keyof ToolDimensions, 'diameter'>; label: string }[] = [
  { key: 'shankDiameter', label: 'Shank diameter' }, { key: 'cuttingLength', label: 'Cutting length' },
  { key: 'overallLength', label: 'Overall length' }, { key: 'stickout', label: 'Stick-out' },
  { key: 'tipAngle', label: 'Tip angle' },
]
const shape = reactive({ shankDiameter: '', cuttingLength: '', overallLength: '', stickout: '', tipAngle: '' })
let precise: ToolDimensions | null = null
const initialShape = { ...shape }
function fill(d: ToolDimensions): void {
  precise = { ...d }
  for (const { key } of fields) shape[key] = initialShape[key] = key === 'tipAngle' ? String(d[key]) : text(d[key])
}
function resetShape(): void {
  if (family.value === 'none') return
  const tool = parsed.value ?? { number: 1, pocket: 1, length: 0, diameter: 0, comment: '' }
  const model = resolveToolModel(tool, { identity: toolIdentity(tool), family: family.value, overrides: {} }, toolModels.defaults)
  if (model !== null) fill(model.dimensions)
}
if (savedModel !== null) fill(savedModel.dimensions); else resetShape()
const model = computed((): ToolModel | null => {
  if (family.value === 'none') return null
  const dimensions: ToolDimensions = { diameter: parsed.value?.diameter ?? NaN, cuttingLength: NaN, shankDiameter: NaN, overallLength: NaN, stickout: NaN, tipAngle: NaN }
  for (const { key } of fields) dimensions[key] = precise !== null && shape[key] === initialShape[key] ? precise[key]
    : (key === 'tipAngle' ? parseNumber(shape[key]) : parseLength(shape[key], unit)) ?? NaN
  return { family: family.value, dimensions, nominalDiameter: false, customized: true }
})
const shapeError = computed(() => model.value === null ? null : toolDimensionError(model.value.family, model.value.dimensions))
const modelBlock = computed(() => saving.value ? null : toolModelSettings.block(modelContext))
const replaces = computed(() => original.value === null && panel.toolTable?.tools.some((tool) => tool.number === parsed.value?.number))
async function save(): Promise<void> {
  const tool = parsed.value
  const preview = model.value
  if (tool === null || shapeError.value !== null || block.value !== null || modelBlock.value !== null || saving.value) return
  saving.value = true; feedback.value = ''
  let tableWritten = false
  if (!sameTool(original.value, tool)) {
    const reason = await saveTool(tool, pendingSave.signal)
    if (reason !== null) { saving.value = false; feedback.value = reason; return }
    original.value = { ...tool }; tableWritten = true
    initial.length = form.length; initial.diameter = form.diameter
  }
  if (!open) return
  // Binding uses the freshly acknowledged row, including a newly edited diameter/description.
  const { cuttingLength, shankDiameter, overallLength, stickout, tipAngle } = preview?.dimensions ?? toolModels.defaults['end-mill']
  const result = await toolModelSettings.saveAssignment(tool, preview?.family ?? null,
    preview === null ? {} : { cuttingLength, shankDiameter, overallLength, stickout, tipAngle }, modelContext)
  saving.value = false
  if (result.saved) { if (isOpen()) emit('close') }
  else feedback.value = `${tableWritten ? 'Tool details saved; preview shape was not saved. ' : ''}${result.reason}`
}
function remove(): void {
  if (original.value === null || block.value !== null || panel.status?.tool.in_spindle === original.value.number) return
  send({ type: 'remove_tool', number: original.value.number }); emit('close')
}
</script>

<template>
  <ModalDialog v-if="deleting" key="delete" :title="`Remove tool ${form.number}?`" size="small" expanded fresh-gesture @close="deleting = false">
    <p>Remove T{{ form.number }} from the tool table? Programs that call it will stop with an error.</p>
    <template #footer><button class="btn" type="button" @click="deleting = false">Cancel</button><button class="btn danger" type="button" :disabled="block !== null || panel.status?.tool.in_spindle === original?.number" @click="remove">Remove tool</button></template>
  </ModalDialog>
  <ModalDialog v-else key="edit" :title="original === null ? 'Add tool' : `Edit tool ${form.number}`" size="large" contained expanded @close="emit('close')">
    <div class="tool-editor">
      <div class="preview"><ToolModelCanvas v-if="model !== null && shapeError === null" :model="model" /><p v-else class="hint">{{ family === 'none' ? 'Choose a preview shape.' : 'Enter valid dimensions to preview the tool.' }}</p></div>
      <div class="details">
        <div class="tool-fields">
          <label class="entry"><span class="label">Tool number</span><input v-model="form.number" v-numeric="'integer'" class="field mono" type="text" inputmode="numeric" :disabled="original !== null || saving" /></label>
          <label class="entry"><span class="label">Length, {{ unit }}</span><input v-model="form.length" v-numeric :data-numeric-unit="unit" class="field mono" type="text" inputmode="decimal" :disabled="saving" /></label>
          <label class="entry"><span class="label">Diameter, {{ unit }}</span><input v-model="form.diameter" v-numeric :data-numeric-unit="unit" class="field mono" type="text" inputmode="decimal" :disabled="saving" /></label>
          <label class="entry description-entry"><span class="label">Description</span><input v-model="form.comment" class="field" type="text" :disabled="saving" /></label>
        </div>
        <div class="shape-heading"><h3>Preview shape</h3><button v-if="family !== 'none'" class="btn quiet" type="button" :disabled="saving" @click="resetShape">Use template</button></div>
        <label class="entry type-entry"><span class="label">Tool type</span><select v-model="family" class="field" :disabled="saving" @change="resetShape"><option value="none">No preview shape</option><option v-for="kind in toolFamilies" :key="kind.id" :value="kind.id">{{ kind.name }}</option></select></label>
        <template v-if="family !== 'none'">
          <p class="diameter-source hint">Cutting diameter: {{ parsed === null ? '—' : text(parsed.diameter) }} {{ unit }} · from Diameter above</p>
          <div class="dimensions"><label v-for="field in fields.filter((item) => item.key !== 'tipAngle' || pointedTool(family as ToolFamily))" :key="field.key" class="entry"><span class="label">{{ field.label }}, {{ field.key === 'tipAngle' ? '°' : unit }}</span><input v-model="shape[field.key]" v-numeric :data-numeric-unit="field.key === 'tipAngle' ? '°' : unit" class="field mono" type="text" inputmode="decimal" :disabled="saving" /></label></div>
          <p class="hint">Shape dimensions affect the preview. Length above sets the machine’s tool offset.</p>
        </template>
      </div>
    </div>
    <p v-if="feedback || block || modelBlock || shapeError || replaces || saving" class="feedback" role="status">{{ feedback || block || modelBlock || shapeError || (replaces ? `Tool ${form.number} exists. Saving replaces it.` : '') || 'Saving… Closing does not cancel a submitted save.' }}</p>
    <template #footer><button v-if="original !== null" class="btn danger" type="button" :disabled="saving || block !== null || panel.status?.tool.in_spindle === original.number" @click="deleting = true">Delete</button><span class="spacer"></span><button class="btn" type="button" @click="emit('close')">{{ saving ? 'Close' : 'Cancel' }}</button><button class="btn primary" type="button" :disabled="saving || parsed === null || block !== null || modelBlock !== null || shapeError !== null" @click="save">{{ saving ? 'Saving…' : 'Save tool' }}</button></template>
  </ModalDialog>
</template>

<style scoped>
.tool-editor { display: grid; grid-template-columns: minmax(220px, .8fr) minmax(0, 1.6fr); gap: 20px; flex: 1; min-height: 0; }
.preview { min-height: 0; min-width: 0; }.preview > p { display: grid; place-items: center; height: 100%; margin: 0; padding: 16px; background: var(--bg-sunken); border-radius: 8px; text-align: center; }
.details { min-width: 0; overflow: auto; padding-right: 4px; display: flex; flex-direction: column; gap: 8px; }
.tool-fields, .dimensions { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 8px 12px; }
.entry { display: flex; flex-direction: column; gap: 5px; min-width: 0; }.entry .field { width: 100%; min-width: 0; min-height: 44px; }.description-entry { grid-column: 1 / -1; }
.shape-heading { display: flex; align-items: center; justify-content: space-between; border-top: 1px solid var(--line); padding-top: 4px; }.shape-heading h3 { font-size: 16px; margin: 0; }.shape-heading .btn { min-height: 36px; }
.type-entry { flex-direction: row; align-items: center; }.type-entry .label { flex: none; width: 80px; }
.hint, .feedback { margin: 0; font-size: 12px; line-height: 1.4; }.feedback { color: var(--warn); }.spacer { flex: 1; }
@media (max-width: 850px) { .tool-editor { grid-template-columns: minmax(160px, .65fr) minmax(0, 1.6fr); gap: 12px; }.dimensions { grid-template-columns: repeat(2, minmax(0, 1fr)); } }
</style>
