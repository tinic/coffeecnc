<script setup lang="ts">
// LinuxCNC is waiting for a manual tool change. The prompt covers the workspace, not the status
// bar or the run bar: stopping stays possible.
import { computed } from 'vue'

import { findTool, linkBlock, panel, send } from '../machine'
import { formatLength } from '../units'
import AppIcon from './AppIcon.vue'

const props = defineProps<{ tool: number }>()

const row = computed(() => findTool(props.tool))
const loaded = computed(() => panel.status?.tool.in_spindle ?? 0)
const loadedRow = computed(() => findTool(loaded.value))
const unit = computed(() => panel.unit)
const spindle = computed(() => panel.status?.spindle.direction ?? 'stopped')
</script>

<template>
  <div class="scrim">
    <section class="prompt" role="alertdialog" aria-label="Tool change">
      <span class="label">The program is waiting for a tool change</span>
      <template v-if="tool === 0">
        <h2>Remove the tool from the spindle</h2>
        <p class="what">The program asks for an empty spindle (T0).</p>
      </template>
      <template v-else>
        <h2>
          Load <span class="mono">T{{ tool }}</span>
        </h2>
        <p v-if="row" class="what">{{ row.comment || 'No description in the tool table' }}</p>
        <p v-else class="what missing">T{{ tool }} is not in the tool table.</p>
        <p v-if="row" class="table mono">
          Table: length {{ formatLength(row.length, unit) }} {{ unit }} · diameter {{ formatLength(row.diameter, unit) }}
          {{ unit }}
        </p>
      </template>
      <p class="now">
        In the spindle now, as LinuxCNC reports:
        <strong v-if="loaded === 0">no tool</strong>
        <strong v-else><span class="mono">T{{ loaded }}</span> {{ loadedRow?.comment }}</strong>
        · spindle {{ spindle === 'stopped' ? 'stopped' : `running ${spindle}` }}
      </p>
      <button
        class="btn primary confirm"
        type="button"
        :disabled="linkBlock !== null"
        :title="linkBlock ?? 'Tell LinuxCNC the tool change is done. The program continues.'"
        @click="send({ type: 'tool_changed' })"
      >
        <AppIcon name="check" />{{ tool === 0 ? 'Spindle is empty' : 'Tool is in the spindle' }}
      </button>
      <p class="hint">
        <template v-if="linkBlock">{{ linkBlock }}: the change cannot be confirmed.</template>
        <template v-else>The program continues at once when you confirm. Stop, in the run bar, ends the program instead.</template>
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

.confirm {
  min-height: 64px;
  margin-top: 8px;
  font-size: 18px;
}
</style>
