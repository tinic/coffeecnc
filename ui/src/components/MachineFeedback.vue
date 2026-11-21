<script setup lang="ts">
import { computed } from 'vue'
import { panel } from '../machine'
import { machineFeedback } from '../machine-feedback'

defineProps<{ showProbe?: boolean }>()
const feedback = computed(() => machineFeedback(panel.status, panel.link))
const showAtSpeed = computed(() => feedback.value.atSpeed.known &&
  typeof panel.status?.spindle.at_speed === 'boolean')
</script>

<template>
  <dl
    v-if="feedback.rpm.known || showAtSpeed || (showProbe && feedback.probe.known)"
    class="feedback" aria-label="Machine feedback"
  >
    <div v-if="feedback.rpm.known">
      <dt>Measured spindle</dt>
      <dd class="rpm mono" :class="{ stale: feedback.rpm.stale }">{{ feedback.rpm.text }}</dd>
    </div>
    <div v-if="showAtSpeed">
      <dt>At speed</dt>
      <dd class="at-speed mono" :class="{ stale: feedback.atSpeed.stale }">{{ feedback.atSpeed.text }}</dd>
    </div>
    <div v-if="showProbe && feedback.probe.known">
      <dt>Probe input</dt>
      <dd class="probe mono" :class="{ stale: feedback.probe.stale, tripped: feedback.probeTripped }">{{ feedback.probe.text }}</dd>
    </div>
  </dl>
</template>

<style scoped>
.feedback { display: flex; flex-wrap: wrap; column-gap: 16px; row-gap: 6px; margin: 8px 0 0; font-size: 12px; color: var(--text-dim); }
.feedback > div { display: flex; align-items: baseline; gap: 8px; }
dd { flex: none; margin: 0; color: var(--text); font-weight: 500; white-space: nowrap; }
dd.rpm { min-width: 10ch; }
dd.at-speed { min-width: 3ch; }
dd.stale, dd.tripped { color: var(--warn); }
</style>
