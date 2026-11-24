<script setup lang="ts">
// The grid of a Z height map (#247): a rectangle in the active work system, a step and the
// clearance Z the probe traverses at. The controller checks every point against the travel.
import { computed, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { panel } from '../machine'
import { heightGrid, heightMapSeconds, maxHeightMapPoints, maxHeightMapSeconds, type HeightMapGrid } from '../height-map'
import { parseNumericExpression } from '../numeric-expression'
import { vNumeric } from '../numeric-input'
import { formatLength, fromDisplay, wcsName } from '../units'
import ModalDialog from './ModalDialog.vue'

const emit = defineEmits<{ start: [grid: HeightMapGrid]; close: [] }>()
const { t } = useI18n()

const unit = computed(() => panel.unit === 'inch' ? 'in' : 'mm')
const work = (axis: 'x' | 'y' | 'z'): number => panel.status?.axes.find((entry) => entry.axis === axis)?.work ?? 0
const initial = (mm: number): string => formatLength(mm, panel.unit)
const fields = ['x_min', 'x_max', 'y_min', 'y_max', 'step', 'clearance'] as const
type Field = typeof fields[number]
// From where the probe is: 40 × 30 mm at 10 mm, traversing at the present height.
const text = ref<Record<Field, string>>({
  x_min: initial(work('x')), x_max: initial(work('x') + 40), y_min: initial(work('y')), y_max: initial(work('y') + 30),
  step: initial(10), clearance: initial(work('z')),
})
const values = computed((): Record<Field, number | null> => Object.fromEntries(fields.map((field) => {
  const value = parseNumericExpression(text.value[field], panel.unit)
  return [field, value === null ? null : fromDisplay(value, panel.unit)]
})) as Record<Field, number | null>)
const grid = computed(() => {
  const { x_min: x0, y_min: y0, x_max: x1, y_max: y1, step } = values.value
  return x0 === null || y0 === null || x1 === null || y1 === null || step === null ? null : heightGrid([x0, y0, x1, y1], step)
})
// The worst case the controller refuses above (fixed_setter.hpp height_map_seconds), with the
// first feed down from the present Z to the clearance.
const worst = computed(() => {
  const clearance = values.value.clearance
  if (grid.value === null || clearance === null) return 0
  return heightMapSeconds(panel.hello?.fixed_setter, grid.value.columns * grid.value.rows, work('z') - clearance)
})
// Without the latch limits the controller decides.
const tooLong = computed(() => grid.value !== null && Number.isFinite(worst.value) && worst.value > maxHeightMapSeconds)
const valid = computed(() => grid.value !== null && values.value.clearance !== null && !tooLong.value)
const labels: Record<Field, () => string> = {
  x_min: () => t('probe.heightMap.xFrom'), x_max: () => t('probe.heightMap.xTo'),
  y_min: () => t('probe.heightMap.yFrom'), y_max: () => t('probe.heightMap.yTo'),
  step: () => t('probe.heightMap.step'), clearance: () => t('probe.heightMap.clearance'),
}
// About 8 s a point with the slow latch on the simulated mill.
const minutes = computed(() => grid.value === null ? 0 : Math.ceil(grid.value.columns * grid.value.rows * 8 / 60))
function start(): void {
  const { x_min, y_min, x_max, y_max, step, clearance } = values.value
  if (!valid.value || x_min === null || y_min === null || x_max === null || y_max === null || step === null || clearance === null) return
  emit('start', { x_min, y_min, x_max, y_max, step, clearance })
}
</script>

<template>
  <ModalDialog :title="t('probe.heightMap.title')" size="medium" fresh-gesture @close="emit('close')">
    <p class="setup">{{ t('probe.heightMap.setup', { wcs: wcsName(panel.status?.offsets.wcs ?? 1) }) }}</p>
    <div class="grid-fields">
      <label v-for="field in fields" :key="field">
        <span>{{ labels[field]() }}</span>
        <input
          v-model="text[field]" v-numeric type="text" class="field mono" inputmode="decimal" autocomplete="off"
          :data-numeric-unit="unit" :data-numeric-action="t('probe.guideDialog.setValue')" :aria-label="labels[field]()"
        />
      </label>
    </div>
    <p class="summary" role="status">
      {{ !grid ? t('probe.heightMap.invalid', { max: maxHeightMapPoints })
        : tooLong ? t('probe.heightMap.tooLong', { minutes: Math.ceil(worst / 60), limit: maxHeightMapSeconds / 60 })
          : t('probe.heightMap.points', { columns: grid.columns, rows: grid.rows, minutes }) }}
    </p>
    <template #footer>
      <button class="btn" type="button" @click="emit('close')">{{ t('common.cancel') }}</button>
      <button class="btn primary start-map" type="button" :disabled="!valid" @click="start">{{ t('probe.guideDialog.startProbe') }}</button>
    </template>
  </ModalDialog>
</template>

<style scoped>
p { margin: 0; line-height: 1.45; font-size: 13px; }
.grid-fields { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 8px 12px; margin: 10px 0; }
.grid-fields label { display: grid; grid-template-columns: auto 96px; align-items: center; gap: 8px; font-size: 12px; }
.grid-fields input { height: 44px; min-width: 0; padding-inline: 8px; }
.summary { color: var(--text-dim); }
.start-map { min-width: 132px; min-height: 44px; }
footer .btn { min-height: 44px; }
</style>
