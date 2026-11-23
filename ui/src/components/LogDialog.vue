<script setup lang="ts">
import { vTooltip } from '../tooltip'
// Everything the operator has been told in this session, newest first. On the simulator it also
// holds the link-fault test control.
import { computed } from 'vue'
import { useI18n } from 'vue-i18n'

import { formatTime } from '../i18n'
import { translateMessage } from '../i18n/messages'

import { panel, setSimulatorFault } from '../machine'
import type { LinkFault } from '../sim/machine'
import AppIcon from './AppIcon.vue'
import ModalDialog from './ModalDialog.vue'

const { t } = useI18n()
const emit = defineEmits<{ close: [] }>()

const entries = computed(() => [...panel.log].reverse())

const faults = computed(() => [
  ['none', t('status.log.faultNone'), t('status.log.faultNoneTip')],
  ['silent', t('status.log.faultSilent'), t('status.log.faultSilentTip')],
  ['stale', t('status.log.faultStale'), t('status.log.faultStaleTip')],
  ['down', t('status.log.faultDown'), t('status.log.faultDownTip')],
] as const satisfies readonly (readonly [LinkFault, string, string])[])
</script>

<template>
  <ModalDialog :title="t('status.log.title')" size="large" @close="emit('close')">
    <p v-if="entries.length === 0" class="hint">{{ t('status.log.empty') }}</p>
    <ul class="log">
      <li v-for="entry in entries" :key="entry.id" :class="entry.level">
        <AppIcon :name="entry.level === 'error' ? 'warning' : 'info'" />
        <span class="when mono">{{ formatTime(entry.time) }}</span>
        <span class="text">
          {{ translateMessage(entry.text) }}<template v-if="entry.count > 1"> (×{{ entry.count }})</template>
          <span v-if="entry.context" class="context">{{ t('status.log.answerTo', { context: translateMessage(entry.context) }) }}</span>
        </span>
      </li>
    </ul>
    <template v-if="panel.simulated" #footer>
      <div class="fault">
        <span class="label">{{ t('status.log.simulatorControl') }}</span>
        <div class="segmented">
          <button
            v-for="[fault, label, title] in faults"
            :key="fault"
            v-tooltip="title"
            class="btn"
            type="button"
            :class="{ on: panel.simulatorFault === fault }"
            @click="setSimulatorFault(fault)"
          >
            {{ label }}
          </button>
        </div>
      </div>
    </template>
  </ModalDialog>
</template>

<style scoped>
.log {
  display: flex;
  flex-direction: column;
  gap: 2px;
  margin: 0;
  padding: 0;
  list-style: none;
}

li {
  display: flex;
  align-items: flex-start;
  gap: 10px;
  padding: 8px 10px;
  border-radius: var(--radius);
  color: var(--text-dim);
}

li.error {
  background: var(--stop-soft);
  color: var(--stop);
}

.when {
  flex: none;
  min-width: 8ch;
  padding-top: 1px;
  font-size: 13px;
  color: var(--text-dim);
}

.text {
  display: flex;
  flex: 1;
  flex-direction: column;
  gap: 2px;
  color: var(--text);
  user-select: text;
  -webkit-user-select: text;
}

.context {
  font-size: 12px;
  color: var(--text-dim);
}

.fault {
  display: flex;
  flex: 1;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
}

.fault .btn {
  padding: 0 12px;
  font-size: 13px;
}
</style>
