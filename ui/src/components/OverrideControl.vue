<script setup lang="ts">
// One override. The number and the slider show what the machine reports; moving the slider only
// asks for a new value.
import { computed } from 'vue'

import { send } from '../machine'
import { formatPercent } from '../units'
import AppIcon from './AppIcon.vue'

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

function nudge(direction: -1 | 1): void {
  // To the next multiple of 10 %.
  const tens = percent.value / 10
  const next = direction > 0 ? Math.floor(tens + 1e-9) + 1 : Math.ceil(tens - 1e-9) - 1
  set(next / 10)
}

function slide(event: Event): void {
  set(Number((event.target as HTMLInputElement).value) / 100)
}
</script>

<template>
  <div class="override" :class="{ off: disabled }">
    <div class="top">
      <span class="label">{{ label }}</span>
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
      <button class="btn square" type="button" :disabled="disabled" :title="`${label} override down`" @click="nudge(-1)">
        <AppIcon name="minus" />
      </button>
      <input
        type="range"
        :min="Math.round(min * 100)"
        :max="Math.round(max * 100)"
        step="1"
        :value="percent"
        :disabled="disabled"
        :aria-label="`${label} override`"
        @input="slide"
      />
      <button class="btn square" type="button" :disabled="disabled" :title="`${label} override up`" @click="nudge(1)">
        <AppIcon name="plus" />
      </button>
    </div>
  </div>
</template>

<style scoped>
.override {
  display: flex;
  flex: 1 1 0;
  flex-direction: column;
  gap: 2px;
  min-width: 0;
}

.top {
  display: flex;
  align-items: baseline;
  justify-content: space-between;
  height: 26px;
}

.value {
  padding: 0 4px;
  border: 0;
  background: none;
  font-size: 19px;
  font-weight: 600;
  cursor: pointer;
}

.value:disabled {
  cursor: default;
}

.off .value {
  color: var(--text-faint);
}

.row {
  display: flex;
  align-items: center;
  gap: 4px;
}

input {
  flex: 1;
}
</style>
