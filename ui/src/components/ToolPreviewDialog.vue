<script setup lang="ts">
import { computed, defineAsyncComponent } from 'vue'
import { useI18n } from 'vue-i18n'
import { panel } from '../machine'
import type { Tool } from '../protocol'
import { holderLabel } from '../tool-holder'
import { resolveToolModel, toolDimensionError, toolFamilyName, toolIdentity } from '../tool-model'
import { defaultToolName, savedProbe, toolModels } from '../tool-models'
import { formatLength, toolLabel } from '../units'
import ModalDialog from './ModalDialog.vue'

// Look at a tool without editing it: the 3D preview and its table values.
const ToolModelCanvas = defineAsyncComponent(() => import('./ToolModelCanvas.vue'))
const props = defineProps<{ tool: Tool }>()
const { t } = useI18n()
const emit = defineEmits<{ close: [] }>()
const unit = computed(() => panel.unit === 'inch' ? 'in' : 'mm')
const model = computed(() => {
  const tool = props.tool
  const probe = savedProbe(tool.number)
  const assignment = toolModels.assignments.get(tool.number) ??
    (probe !== undefined ? { identity: toolIdentity(tool), family: probe.family, overrides: probe.overrides,
      ...(probe.holder === undefined ? {} : { holder: probe.holder }) }
      : tool.number === panel.hello?.touch_probe_tool ? { identity: toolIdentity(tool), family: 'touch-probe' as const, overrides: {} } : undefined)
  const resolved = resolveToolModel(tool, assignment, toolModels.defaults)
  return resolved !== null && toolDimensionError(resolved.family, resolved.dimensions) === null ? resolved : null
})
const kind = computed(() => model.value === null ? t('tools.shared.noPreviewShape') : toolFamilyName(model.value.family))
const probe = computed(() => model.value?.family === 'touch-probe')
</script>

<template>
  <ModalDialog :title="`${toolLabel(tool.number)} ${tool.comment || defaultToolName(tool.number)}`" size="large" contained expanded @close="emit('close')">
    <div class="tool-preview">
      <div class="preview"><ToolModelCanvas v-if="model !== null" :model="model" /><p v-else class="hint">{{ t('tools.preview.noShape') }}</p></div>
      <dl class="values">
        <dt>{{ t('tools.preview.type') }}</dt><dd>{{ kind }}</dd>
        <template v-if="model?.holder"><dt>{{ t('tools.shared.holder') }}</dt><dd>{{ holderLabel(model.holder) }}</dd></template>
        <dt>{{ t('tools.preview.length') }}</dt><dd class="mono">{{ formatLength(tool.length, panel.unit) }} {{ unit }}</dd>
        <dt>{{ probe ? t('tools.dimension.ballDiameter') : t('tools.dimension.diameter') }}</dt><dd class="mono">{{ formatLength(tool.diameter, panel.unit) }} {{ unit }}</dd>
        <dt>{{ t('tools.preview.pocket') }}</dt><dd class="mono">{{ tool.pocket }}</dd>
      </dl>
    </div>
    <template #footer><span class="spacer"></span><button class="btn primary" type="button" @click="emit('close')">{{ t('common.close') }}</button></template>
  </ModalDialog>
</template>

<style scoped>
.tool-preview { display: grid; grid-template-columns: minmax(0, 1fr) 240px; gap: 20px; height: 100%; min-height: 0; }
.preview { min-height: 0; min-width: 0; }
.preview > p { display: grid; place-items: center; height: 100%; margin: 0; padding: 16px; background: var(--bg-sunken); border-radius: 8px; text-align: center; }
.values { display: grid; grid-template-columns: auto 1fr; align-content: start; gap: 10px 14px; margin: 0; font-size: 14px; }
.values dt { color: var(--text-dim); }
.values dd { margin: 0; }
.spacer { flex: 1; }
</style>
