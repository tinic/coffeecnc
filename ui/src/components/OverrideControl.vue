<script setup lang="ts">
// One override. The number and the slider show what the machine reports; dragging shows the value
// under the finger and asks for it only on release (turo: the slider jumped while the reports of
// each intermediate value came back). A release back at the machine's value fires no change event,
// so the finger lifting also ends the drag; a cancelled drag asks for nothing.
import { computed, ref } from 'vue'
import { useI18n } from 'vue-i18n'

import { send } from '../machine'
import type { SetOverride } from '../protocol'
import { formatPercent } from '../units'

const props = defineProps<{
  label: string
  which: SetOverride['which']
  /** The override the machine reports, or null without status. */
  value: number | null
  min: number
  max: number
  disabled: boolean
}>()

const { t } = useI18n()
const resetLabels = {
  feed: 'manual.override.resetFeed',
  rapid: 'manual.override.resetRapid',
  spindle: 'manual.override.resetSpindle',
  velocity_limit: 'manual.override.resetVelocityLimit',
} as const

/** The percent under the finger while dragging; null when the slider shows the machine. */
const dragging = ref<number | null>(null)
const percent = computed(() => dragging.value ?? Math.round((props.value ?? 1) * 100))

function set(value: number): void {
  send({ type: 'set_override', which: props.which, value: Math.min(props.max, Math.max(props.min, value)) })
}

function slide(event: Event): void {
  dragging.value = Number((event.target as HTMLInputElement).value)
}

/** Asks for the value under the finger, once, unless the machine already has it. */
function release(): void {
  const asked = dragging.value
  dragging.value = null
  if (asked !== null && asked !== Math.round((props.value ?? 1) * 100)) set(asked / 100)
}

function reset(): void {
  dragging.value = null
  set(1)
}
</script>

<template>
  <div class="override" :class="{ off: disabled }">
    <div class="top">
      <span class="caption">{{ label }}</span>
      <span class="value mono">
        {{ dragging !== null ? formatPercent(dragging / 100) : value === null ? '—' : formatPercent(value) }}
      </span>
      <button class="reset" type="button" :aria-label="t(resetLabels[which])" :disabled="disabled || value === null || value === 1 || min > 1 || max < 1" @click="reset">{{ t('manual.override.reset') }}</button>
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
        @change="release"
        @pointerup="release"
        @pointercancel="dragging = null"
      />
    </div>
  </div>
</template>

<style scoped>
.override {
  display: grid;
  grid-template-columns: auto minmax(50px, 1fr) 48px 86px;
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
  min-width: 48px;
  text-align: right;
  padding: 0 4px;
  border: 0;
  background: none;
  font-size: 12px;
  font-weight: 500;
}
.reset { grid-column: 4; grid-row: 1; min-height: 44px; padding: 0 6px; border: 1px solid var(--line-strong); border-radius: 7px; background: var(--control); color: var(--text); font-size: 11px; cursor: pointer; }
.reset:disabled { color: var(--text-faint); cursor: default; }.reset:hover:not(:disabled) { background: var(--accent-soft); }

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
