<script setup lang="ts">
import { computed } from 'vue'
import { panel } from '../machine'
import { machineFeedback } from '../machine-feedback'

defineProps<{ showProbe?: boolean }>()
const feedback = computed(() => machineFeedback(panel.status, panel.link))
</script>

<template>
  <dl class="feedback" aria-label="Machine feedback">
    <div><dt>Measured spindle</dt><dd :class="{ unknown: !feedback.rpm.known, stale: feedback.rpm.stale }">{{ feedback.rpm.text }}</dd></div>
    <div><dt>At speed</dt><dd :class="{ unknown: !feedback.atSpeed.known, stale: feedback.atSpeed.stale }">{{ feedback.atSpeed.text }}</dd></div>
    <div v-if="showProbe"><dt>Probe input</dt><dd :class="{ unknown: !feedback.probe.known, stale: feedback.probe.stale, tripped: feedback.probeTripped }">{{ feedback.probe.text }}</dd></div>
  </dl>
</template>

<style scoped>
.feedback { display: flex; flex-wrap: wrap; column-gap: 24px; row-gap: 6px; margin: 8px 0 0; font-size: 12px; color: var(--text-dim); }
.feedback > div { display: flex; align-items: baseline; gap: 8px; }
dd { margin: 0; color: var(--text); font-weight: 500; }
dd.unknown { color: var(--text-dim); font-weight: 400; }
dd.stale, dd.tripped { color: var(--warn); }
</style>
