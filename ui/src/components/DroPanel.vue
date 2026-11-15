<script setup lang="ts">
import { vTooltip } from '../tooltip'
import { computed, ref } from 'vue'
import { startDialog } from '../dialogs'
import { axes, live, mdiBlock, panel, reported, send } from '../machine'
import type { AxisLetter, AxisStatus } from '../protocol'
import { formatLength, fromDisplay, wcsName } from '../units'
import AppIcon from './AppIcon.vue'
import NumberDialog from './NumberDialog.vue'

const basis = ref<'work' | 'machine' | 'dtg'>('work')
const unit = computed(() => panel.unit)
const wcs = computed(() => wcsName(reported.value?.offsets.wcs ?? 1))
const touching = ref<AxisLetter | null>(startDialog === 'touchoff' ? 'x' : null)
const shifted = computed(() => reported.value?.offsets.g92.some((v) => v !== 0) ?? false)
function cycleBasis(): void { basis.value = basis.value === 'work' ? 'machine' : basis.value === 'machine' ? 'dtg' : 'work' }
function marker(axis: AxisStatus): string {
  if (axis.homing) return 'Homing'
  if (axis.on_hard_limit) return 'On limit'
  return axis.homed ? '' : 'Not homed'
}
function touchOff(value: number): void {
  if (touching.value !== null) send({ type: 'touch_off', axis: touching.value, value: fromDisplay(value, unit.value) })
  touching.value = null
}
</script>

<template>
  <section class="dro" :class="{ distrust: !live }" aria-label="Coordinate readout">
    <button v-tooltip="'Switch between work position, machine position and distance to go'" class="basis" type="button" @click="cycleBasis">
      <strong>{{ basis === 'work' ? wcs : basis === 'machine' ? 'Machine' : 'To go' }}</strong>
      <span>{{ basis === 'work' ? 'Work position' : basis === 'machine' ? 'Position' : 'Remaining' }}<template v-if="shifted && basis === 'work'"> · G92</template></span>
    </button>
    <div v-for="axis in axes" :key="axis.axis" class="axis">
      <span class="letter mono">{{ axis.axis.toUpperCase() }}</span>
      <div class="reading"><span class="number mono">{{ formatLength(axis[basis], unit) }}</span><span class="marker">{{ !live ? 'Last reported' : marker(axis) }}</span></div>
      <button v-if="panel.tab === 'manual'" v-tooltip="mdiBlock ?? `Touch off ${axis.axis.toUpperCase()} in ${wcs}`" class="touch" type="button" :disabled="mdiBlock !== null" :aria-label="`Touch off ${axis.axis.toUpperCase()} in ${wcs}`" @click="touching = axis.axis"><AppIcon name="target" /></button>
    </div>
    <span v-if="axes.length === 0" class="unavailable">Position unavailable</span>
    <div class="units" role="group" aria-label="Display units">
      <button type="button" :aria-pressed="unit === 'mm'" @click="panel.unit = 'mm'">mm</button>
      <button type="button" :aria-pressed="unit === 'inch'" @click="panel.unit = 'inch'">in</button>
    </div>
    <NumberDialog v-if="touching !== null" :title="`Touch off ${touching.toUpperCase()} in ${wcs}`" :effect="`Changes the ${wcs} work offset so that ${touching.toUpperCase()} reads the value below at the present position. The machine does not move.`" :label="`${touching.toUpperCase()} is now at`" :unit="unit" initial="0" :action="`Set ${wcs} ${touching.toUpperCase()}`" @confirm="touchOff" @close="touching = null" />
  </section>
</template>

<style scoped>
.dro { display: flex; align-items: center; flex: none; min-height: 78px; margin: 0 24px 10px; padding: 12px 0; border: 1px solid var(--line); border-radius: 12px; background: var(--panel); }
.basis { display: flex; flex-direction: column; gap: 5px; width: 125px; flex: none; padding: 0 20px; border: 0; background: none; text-align: left; cursor: pointer; }
.basis strong { font-size: 13px; font-weight: 500; }.basis span { font-size: 10px; color: var(--text-dim); white-space: nowrap; }
.axis { display: flex; flex: 0 1 250px; height: 48px; align-items: center; gap: 12px; min-width: 0; padding: 0 18px; border-left: 1px solid var(--line); }
.letter { flex: none; width: 20px; color: var(--text); }.reading { position: relative; display: flex; flex-direction: column; min-width: 0; }
.number, .letter { font-size: 27px; line-height: 1.3; letter-spacing: -1px; font-weight: 500; white-space: nowrap; }.marker { position: absolute; top: 100%; left: 0; height: 15px; line-height: 15px; white-space: nowrap; font-size: 10px; color: var(--warn); }
.touch { align-self: center; border: 0; background: none; padding: 8px; color: var(--text-dim); cursor: pointer; }.touch:disabled { opacity: .3; }
.units { display: flex; flex: none; gap: 3px; padding: 3px; margin: 0 12px; border: 1px solid var(--line-strong); border-radius: 9px; background: var(--bg-sunken); }
.units button { width: 52px; height: 48px; border: 1px solid transparent; border-radius: 6px; background: none; font-size: 16px; color: var(--text-dim); cursor: pointer; }
.units button[aria-pressed="true"] { background: var(--accent-soft); border-color: var(--accent); color: var(--text); font-weight: 600; }
.distrust .number { color: var(--text-faint); }.unavailable { flex: 1; color: var(--text-dim); }
@media (max-height: 820px) { .dro { min-height: 70px; padding-block: 8px; margin-bottom: 8px; } }
</style>
