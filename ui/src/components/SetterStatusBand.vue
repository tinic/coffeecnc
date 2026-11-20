<script setup lang="ts">
import type { SetterStatusLine } from '../setter-state'
import { vTooltip } from '../tooltip'

defineProps<{ line: SetterStatusLine }>()
</script>

<template>
  <div class="setter-band" :class="line.level" role="status" aria-live="polite">
    <span class="setter-label" aria-hidden="true">{{ line.level === 'blocked' ? '!' : line.level === 'done' ? '✓' : 'i' }}</span>
    <p v-tooltip="line.text">{{ line.text }}</p>
  </div>
</template>

<style scoped>
.setter-band { display: flex; align-items: center; gap: 12px; height: 40px; margin: 6px 0 4px; padding: 0 10px; border: 1px solid var(--line); border-radius: 9px; background: var(--panel); color: var(--text-dim); }
.setter-band.blocked { color: var(--stop); background: var(--stop-soft); border-color: var(--stop); box-shadow: inset 4px 0 var(--stop); }
.setter-band.done { color: var(--go); }
.setter-band.busy { color: var(--accent); }
.setter-label { display: grid; place-items: center; flex: none; width: 24px; height: 24px; border: 1px solid currentColor; border-radius: 50%; font-weight: 700; }
.setter-band p { flex: 1; min-width: 0; margin: 0; font-size: 13px; line-height: 16px; display: -webkit-box; -webkit-line-clamp: 2; -webkit-box-orient: vertical; overflow: hidden; }
</style>
