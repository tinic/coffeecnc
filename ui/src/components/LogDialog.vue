<script setup lang="ts">
import { vTooltip } from '../tooltip'
// Everything the operator has been told in this session, newest first. On the simulator it also
// holds the link-fault test control.
import { computed } from 'vue'

import { panel, setSimulatorFault } from '../machine'
import type { LinkFault } from '../sim/machine'
import { formatClock } from '../units'
import AppIcon from './AppIcon.vue'
import ModalDialog from './ModalDialog.vue'

const emit = defineEmits<{ close: [] }>()

const entries = computed(() => [...panel.log].reverse())

const faults: readonly (readonly [LinkFault, string, string])[] = [
  ['none', 'Working', 'Status arrives normally.'],
  ['silent', 'Silent', 'A frozen executable: no status and no answers.'],
  ['stale', 'Reports stale', 'LinuxCNC stopped writing its status; the last one keeps arriving.'],
  ['down', 'Reports down', 'No connection to LinuxCNC; placeholder status.'],
]
</script>

<template>
  <ModalDialog title="Message log" size="large" @close="emit('close')">
    <p v-if="entries.length === 0" class="hint">No messages in this session.</p>
    <ul class="log">
      <li v-for="entry in entries" :key="entry.id" :class="entry.level">
        <AppIcon :name="entry.level === 'error' ? 'warning' : 'info'" />
        <span class="when mono">{{ formatClock(entry.time) }}</span>
        <span class="text">
          {{ entry.text }}<template v-if="entry.count > 1"> (×{{ entry.count }})</template>
          <span v-if="entry.context" class="context">Answer to: {{ entry.context }}</span>
        </span>
      </li>
    </ul>
    <template v-if="panel.simulated" #footer>
      <div class="fault">
        <span class="label">Simulator test control: link</span>
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
