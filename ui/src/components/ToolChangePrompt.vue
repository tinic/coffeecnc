<script setup lang="ts">
import { vTooltip } from '../tooltip'
// LinuxCNC is waiting for a manual tool change. The prompt covers the workspace, not the status
// bar or the run bar: stopping stays possible.
import { computed } from 'vue'
import { useI18n } from 'vue-i18n'

import { findTool, linkBlock, panel, send } from '../machine'
import { formatLength, toolLabel } from '../units'
import AppIcon from './AppIcon.vue'

const props = defineProps<{ tool: number; measure?: boolean; measuring?: boolean }>()
const emit = defineEmits<{ measure: [] }>()
const { t } = useI18n()
const loadKey = 'tools.change.load'
const nowKey = 'tools.change.now'
const spindleStates = {
  stopped: 'tools.change.spindleStopped', forward: 'tools.change.spindleForward', reverse: 'tools.change.spindleReverse',
} as const

const row = computed(() => findTool(props.tool))
const loaded = computed(() => panel.status?.tool.in_spindle ?? 0)
const loadedRow = computed(() => findTool(loaded.value))
const unit = computed(() => panel.unit)
const spindle = computed(() => panel.status?.spindle.direction ?? 'stopped')
</script>

<template>
  <div class="scrim">
    <section class="prompt" role="alertdialog" :aria-label="t('tools.change.title')" :aria-busy="measuring">
      <span class="label">{{ t('tools.change.waiting') }}</span>
      <template v-if="tool === 0">
        <h2>{{ t('tools.change.removeTool') }}</h2>
        <p class="what">{{ t('tools.change.emptySpindle') }}</p>
      </template>
      <template v-else>
        <i18n-t :keypath="loadKey" tag="h2" scope="global">
          <template #tool><strong class="mono">{{ toolLabel(tool) }}</strong></template>
        </i18n-t>
        <p v-if="row" class="what">{{ row.comment || t('tools.change.noDescription') }}</p>
        <p v-else class="what missing">{{ t('tools.change.notInTable', { tool: toolLabel(tool) }) }}</p>
        <p v-if="row" class="table mono">
          {{ t('tools.change.tableValues', { length: formatLength(row.length, unit), diameter: formatLength(row.diameter, unit), unit }) }}
        </p>
      </template>
      <i18n-t :keypath="nowKey" tag="p" class="now" scope="global">
        <template #tool>
          <strong v-if="loaded === 0">{{ t('tools.change.noTool') }}</strong>
          <strong v-else><span class="mono">{{ toolLabel(loaded) }}</span> {{ loadedRow?.comment }}</strong>
        </template>
        <template #spindle>{{ t(spindleStates[spindle]) }}</template>
      </i18n-t>
      <div class="actions">
        <button
          v-if="measure || measuring" v-tooltip="t('tools.change.measureHint')"
          class="btn confirm" type="button" :disabled="linkBlock !== null || measuring"
          @click="emit('measure')"
        >
          {{ t('tools.change.measure') }}
        </button>
        <button
          v-tooltip="linkBlock ?? t('tools.change.confirmHint')"
          class="btn primary confirm"
          type="button"
          :disabled="linkBlock !== null || measuring"
          @click="send({ type: 'tool_changed' })"
        >
          <AppIcon name="check" />{{ tool === 0 ? t('tools.change.spindleEmpty') : t('tools.change.toolInSpindle') }}
        </button>
      </div>
      <p class="hint">
        <template v-if="measuring">{{ t('tools.change.measuring') }}</template>
        <template v-else-if="linkBlock">{{ t('tools.change.cannotConfirm', { reason: linkBlock }) }}</template>
        <template v-else-if="measure">{{ t('tools.change.measureNote') }}</template>
        <template v-else>{{ t('tools.change.continueNote') }}</template>
      </p>
    </section>
  </div>
</template>

<style scoped>
.scrim {
  position: absolute;
  inset: 0;
  border-radius: 12px;
  z-index: 10;
  display: flex;
  align-items: center;
  justify-content: center;
  background: rgb(8 10 12 / 78%);
  pointer-events: auto;
}

.prompt {
  display: flex;
  flex-direction: column;
  gap: 10px;
  width: 560px;
  max-height: 100%;
  overflow-y: auto;
  padding: 24px 28px;
  border: 2px solid var(--warn);
  border-radius: 12px;
  background: var(--panel-raised);
  box-shadow: 0 18px 50px rgb(0 0 0 / 50%);
}

.scrim.with-status-row { bottom: 34px; }
@media (max-height: 850px) {
  .scrim.with-status-row { bottom: 30px; }
  .prompt { padding: 16px 24px; gap: 8px; }
  .prompt h2 { font-size: 30px; }
}

.label {
  color: var(--warn);
}

h2 {
  margin: 0;
  font-size: 40px;
  font-weight: 600;
  line-height: 1.1;
}

p {
  margin: 0;
}

.what {
  font-size: 20px;
}

.what.missing {
  color: var(--stop);
}

.table,
.now {
  color: var(--text-dim);
}

.now strong {
  font-weight: 600;
  color: var(--text);
}

.actions {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  margin-top: 8px;
}

.confirm {
  flex: 1 1 auto;
  min-height: 64px;
  font-size: 18px;
}
</style>
