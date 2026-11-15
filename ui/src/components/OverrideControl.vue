<script setup lang="ts">
// One override. The number and the slider show what the machine reports; moving the slider only
// asks for a new value.
import { computed } from 'vue'

import { send } from '../machine'
import { formatPercent } from '../units'

const props = defineProps<{
  label: string
  which: 'feed' | 'rapid' | 'spindle'
  /** The override the machine reports, or null without status. */
  value: number | null
  min: number
  max: number
  disabled: boolean
}>()

const percent = computed(() => Math.round((props.value ?? 1) * 100))

function set(value: number): void {
  send({ type: 'set_override', which: props.which, value: Math.min(props.max, Math.max(props.min, value)) })
}

function slide(event: Event): void {
  set(Number((event.target as HTMLInputElement).value) / 100)
}
</script>

<template>
  <div class="override" :class="{ off: disabled }">
    <div class="top">
      <span class="caption">{{ label }}</span>
      <button
        class="value mono"
        type="button"
        :disabled="disabled || percent === 100"
        title="Back to 100 %"
        @click="set(1)"
      >
        {{ value === null ? '—' : formatPercent(value) }}
      </button>
    </div>
    <div class="row">
      <input
        type="range"
        :min="Math.round(min * 100)"
        :max="Math.round(max * 100)"
        step="1"
        :value="percent"
        :disabled="disabled"
        :aria-label="label"
        @input="slide"
      />
    </div>
  </div>
</template>

<style scoped>
.override {
  display: grid;
  grid-template-columns: 110px minmax(0, 1fr) 56px;
  align-items: center;
  flex: 1 1 0;
  gap: 8px;
  min-width: 0;
}

.top {
  display: contents;
}

.value {
  grid-column: 3;
  grid-row: 1;
  min-height: 44px;
  padding: 0 4px;
  border: 0;
  background: none;
  font-size: 12px;
  font-weight: 500;
  cursor: pointer;
}

.value:disabled {
  cursor: default;
}

.off .value {
  color: var(--text-faint);
}

.row {
  grid-column: 2;
  grid-row: 1;
  display: flex;
  align-items: center;
  gap: 4px;
}

input {
  flex: 1;
  height: 28px;
}
.caption { grid-column: 1; grid-row: 1; font-size: 12px; color: var(--text-dim); }
</style>
