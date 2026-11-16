<script setup lang="ts">
import { computed } from 'vue'
import { panel } from '../machine'
import { machineFeedback } from '../machine-feedback'

defineProps<{ showProbe?: boolean }>()
const feedback = computed(() => machineFeedback(panel.status, panel.link))
</script>

<template>
  <dl class="feedback" aria-label="Machine feedback">
    <div><dt>Measured spindle</dt><dd class="rpm mono" :class="{ unknown: !feedback.rpm.known, stale: feedback.rpm.stale }"><template v-if="feedback.rpm.known"><span class="rpm-number">{{ feedback.rpm.text.split(' ')[0] }}</span> rpm<template v-if="feedback.rpm.stale"> · stale</template></template><template v-else>{{ feedback.rpm.text }}</template></dd></div>
    <div><dt>At speed</dt><dd class="at-speed mono" :class="{ unknown: !feedback.atSpeed.known, stale: feedback.atSpeed.stale }">{{ feedback.atSpeed.text }}</dd></div>
    <div v-if="showProbe"><dt>Probe input</dt><dd class="probe mono" :class="{ unknown: !feedback.probe.known, stale: feedback.probe.stale, tripped: feedback.probeTripped }">{{ feedback.probe.text }}</dd></div>
  </dl>
</template>

<style scoped>
.feedback { display: flex; flex-wrap: wrap; column-gap: 16px; row-gap: 6px; margin: 8px 0 0; font-size: 12px; color: var(--text-dim); }
.feedback > div { display: flex; align-items: baseline; gap: 8px; }
dd { flex: none; margin: 0; color: var(--text); font-weight: 500; white-space: nowrap; }
/* Include Unavailable and stale suffixes so neighboring telemetry never follows the digits. */
dd.rpm { width: 20ch; min-width: 20ch; } dd.at-speed { width: 11ch; min-width: 11ch; } dd.probe { width: 15ch; min-width: 15ch; }
.rpm-number { display: inline-block; width: 8ch; min-width: 8ch; text-align: right; }
dd.unknown { color: var(--text-dim); font-weight: 400; }
dd.stale, dd.tripped { color: var(--warn); }
</style>
