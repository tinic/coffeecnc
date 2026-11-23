<script setup lang="ts">
import { computed, defineAsyncComponent, onBeforeUnmount, reactive, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { translateMessage } from '../i18n/messages'
import { panel } from '../machine'
import { vNumeric } from '../numeric-input'
import type { Tool } from '../protocol'
import { dimensionKeys, factoryToolDefaults, pointedTool, readToolDimensions, resolveToolModel, setToolDefault,
  shapeOverrides, toolDimensionError, toolFamilies, toolFamilyName, toolIdentity,
  type DimensionKey, type ToolDimensions, type ToolFamily, type ToolModel } from '../tool-model'
import { toolModelSettings, toolModels } from '../tool-models'
import { parseNumericExpression } from '../numeric-expression'
import { fromDisplay, toDisplay } from '../units'
import ModalDialog from './ModalDialog.vue'

const ToolModelCanvas = defineAsyncComponent(() => import('./ToolModelCanvas.vue'))
const { t } = useI18n()
const props = defineProps<{ tool: Tool | null }>()
const emit = defineEmits<{ close: [] }>()
const defaultsMode = props.tool === null
const editContext = toolModelSettings.context()
const saving = ref(false)
const saveError = ref('')
let open = true
onBeforeUnmount(() => { open = false })
const saveBlock = computed(() => saving.value ? null : toolModelSettings.block(editContext) ??
  (props.tool !== null && !panel.simulated && panel.link !== 'live' ? t('tools.modelDialog.tableRequired') : null))
const scope = computed(() => {
  switch (toolModels.storage) {
    case 'browser': return defaultsMode ? t('tools.modelDialog.scopeBrowserDefaults') : t('tools.modelDialog.scopeBrowserSession')
    case 'available': return t('tools.modelDialog.scopeAvailable', { machine: panel.hello?.machine_id ?? '' })
    case 'session_only': return t('tools.modelDialog.scopeSessionOnly')
    case 'loading': return t('tools.modelDialog.scopeLoading')
    case 'unavailable': return t('tools.modelDialog.scopeUnavailable')
  }
})
const unit = panel.unit // Keep an open editor's unit stable.
const identity = props.tool === null ? '' : toolIdentity(props.tool)
const saved = props.tool === null ? undefined : toolModels.assignments.get(props.tool.number)
const family = ref<ToolFamily>(saved?.identity === identity ? saved.family : 'end-mill')
const custom = ref<boolean>(saved?.identity === identity && Object.keys(saved.overrides).length > 0)
const labels = {
  diameter: defaultsMode ? 'tools.dimension.referenceDiameter' : 'tools.dimension.cuttingDiameter',
  shankDiameter: 'tools.dimension.shankDiameter', cuttingLength: 'tools.dimension.cuttingLength',
  overallLength: 'tools.dimension.overallLength', stickout: 'tools.dimension.stickout',
  tipAngle: 'tools.dimension.includedTipAngle', bodyDiameter: 'tools.dimension.bodyDiameter',
  bodyLength: 'tools.dimension.bodyLength', stylusLength: 'tools.dimension.stylusLength',
} as const
const fields = (Object.keys(labels) as DimensionKey[]).map((key) => ({ key }))
const visibleFields = computed(() => {
  const keys = dimensionKeys(family.value)
  return fields.filter(({ key }) => keys.includes(key) && (key !== 'tipAngle' || pointedTool(family.value)))
})
const form = reactive<Record<DimensionKey, string>>({ diameter: '', cuttingLength: '', shankDiameter: '', overallLength: '', stickout: '', tipAngle: '', bodyDiameter: '', bodyLength: '', stylusLength: '' })
const draftDefaults = reactive(factoryToolDefaults())
for (const { id } of toolFamilies) setToolDefault(draftDefaults, id, { ...toolModels.defaults[id] })
const baseline = computed(() => props.tool === null ? null : resolveToolModel(props.tool, { identity, family: family.value, overrides: {} }, draftDefaults))
function fill(d: ToolDimensions): void {
  for (const key of dimensionKeys(family.value)) {
    const value = Reflect.get(d, key) as number
    form[key] = String(Number((key === 'tipAngle' ? value : toDisplay(value, unit)).toFixed(6)))
  }
}
function load(): void {
  fill(defaultsMode ? draftDefaults[family.value] : baseline.value?.dimensions ?? draftDefaults[family.value])
}
load()
if (custom.value && props.tool !== null && saved !== undefined) {
  const resolved = resolveToolModel(props.tool, saved, draftDefaults)
  if (resolved !== null) fill(resolved.dimensions)
}
function dimensionsOf(kind: ToolFamily): ToolDimensions {
  return readToolDimensions(kind, (key) => {
    if (key === 'diameter' && !defaultsMode && baseline.value !== null) return baseline.value.dimensions.diameter
    const n = parseNumericExpression(form[key], key === 'tipAngle' ? '°' : unit) ?? NaN
    return key === 'tipAngle' ? n : fromDisplay(n, unit)
  })
}
const dimensions = computed(() => dimensionsOf(family.value))
const model = computed((): ToolModel => ({ family: family.value,
  dimensions: !defaultsMode && !custom.value && baseline.value !== null ? baseline.value.dimensions : dimensions.value,
  nominalDiameter: baseline.value?.nominalDiameter ?? true, customized: custom.value }))
const error = computed(() => toolDimensionError(family.value, model.value.dimensions))
const changedTool = computed(() => props.tool !== null && !panel.toolTable?.tools.some((row) => toolIdentity(row) === identity))
watch(family, (_current, previous) => {
  // Preserve valid edits while browsing family defaults; invalid drafts are not stored.
  if (defaultsMode) {
    const d = dimensionsOf(previous)
    if (toolDimensionError(previous, d) === null) setToolDefault(draftDefaults, previous, d)
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
    setToolDefault(draftDefaults, family.value, dimensions.value)
    const copy = factoryToolDefaults()
    for (const { id } of toolFamilies) setToolDefault(copy, id, { ...draftDefaults[id] })
    result = await toolModelSettings.saveDefaults(copy, editContext)
  } else {
    result = await toolModelSettings.saveAssignment(props.tool, family.value, custom.value ? shapeOverrides(model.value.dimensions) : {}, editContext)
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
  <ModalDialog :title="defaultsMode ? t('tools.modelDialog.templatesTitle') : t('tools.modelDialog.modelTitle', { number: tool?.number ?? '' })" size="large" contained expanded @close="emit('close')">
    <div class="editor">
      <div class="settings">
        <label class="entry"><span class="label">{{ t('tools.modelDialog.toolFamily') }}</span>
          <select v-model="family" class="field" :disabled="saving"><option v-for="f in toolFamilies" :key="f.id" :value="f.id">{{ toolFamilyName(f.id) }}</option></select>
        </label>
        <p v-if="defaultsMode" class="hint explanation">{{ family === 'touch-probe' ? t('tools.modelDialog.probeScaling') : t('tools.modelDialog.cutterScaling') }}</p>
        <template v-else>
          <label class="check"><input v-model="custom" type="checkbox" :disabled="saving" />{{ t('tools.modelDialog.override') }}</label>
          <p class="hint explanation">{{ model.nominalDiameter ? t('tools.modelDialog.nominalDiameter') : t('tools.modelDialog.tableDiameter') }} {{ custom ? t('tools.modelDialog.overridesAbsolute') : t('tools.modelDialog.scaledDefaults') }}</p>
        </template>
        <div class="dimensions">
          <label v-for="field in visibleFields" :key="field.key" class="entry">
            <span class="label">{{ field.key === 'diameter' && family === 'touch-probe' ? t('tools.dimension.ballDiameter') : t(labels[field.key]) }} <span class="hint">{{ field.key === 'tipAngle' ? '°' : unit }}</span></span>
            <input v-model="form[field.key]" v-numeric :data-numeric-unit="field.key === 'tipAngle' ? '°' : unit" class="field mono" type="text" inputmode="decimal" :disabled="saving || !defaultsMode && (!custom || field.key === 'diameter')" />
          </label>
        </div>
        <p class="hint explanation">{{ family === 'touch-probe' ? t('tools.modelDialog.stylusExplanation') : t('tools.modelDialog.stickoutExplanation') }}</p>
        <p class="scope hint">{{ scope }} {{ t('tools.modelDialog.simplified') }}</p>
      </div>
      <div class="preview">
        <ToolModelCanvas v-if="error === null" :model="model" />
        <div v-else class="invalid hint">{{ t('tools.shared.enterValidDimensions') }}</div>
      </div>
    </div>
    <p v-if="saveError || saveBlock || changedTool || error || saving" class="feedback" :class="saving ? 'hint' : 'blocked'" role="status">
      {{ translateMessage(saveError) || saveBlock || (changedTool ? t('tools.modelDialog.tableChanged') : error) || t('tools.modelDialog.savingNote') }}
    </p>
    <template #footer>
      <button v-if="defaultsMode" class="btn quiet" type="button" :disabled="saving" @click="reset">{{ t('tools.modelDialog.resetFamily') }}</button>
      <button v-else class="btn quiet" type="button" :disabled="saving || changedTool || saveBlock !== null" @click="clearAssignment">{{ t('tools.modelDialog.clearModel') }}</button>
      <span class="spacer"></span>
      <button class="btn" type="button" @click="emit('close')">{{ saving ? t('common.close') : t('common.cancel') }}</button>
      <button class="btn primary" type="button" :disabled="saving || error !== null || changedTool || saveBlock !== null" @click="save">{{ saving ? t('common.saving') : defaultsMode ? t('tools.modelDialog.saveDefaults') : t('tools.modelDialog.saveModel') }}</button>
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
