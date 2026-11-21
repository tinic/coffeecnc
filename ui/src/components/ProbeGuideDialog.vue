<script setup lang="ts">
import { computed, defineAsyncComponent, ref, useTemplateRef, watch } from 'vue'
import type { FixedSetterLimits } from '../protocol'
import { angleChoices, guideInput, probeGuide, type AngleEdge, type ProbeGuideTarget } from '../probe-guide'
import { parseNumericExpression } from '../numeric-expression'
import { vNumeric } from '../numeric-input'
import ModalDialog from './ModalDialog.vue'

const GuideCanvas = defineAsyncComponent(() => import('./ProbeGuideCanvas.vue'))
const props = defineProps<{
  target: ProbeGuideTarget
  limits: FixedSetterLimits | undefined
  tool?: number
  ruby?: boolean
}>()
const emit = defineEmits<{ start: [value: number, angle: AngleEdge]; close: [] }>()
const angle = ref<AngleEdge>('y_plus')
const input = computed(() => guideInput(props.target))
const text = ref(input.value?.initial ?? '')
const value = computed(() => parseNumericExpression(text.value, 'mm'))
const plan = computed(() => probeGuide(props.target, props.limits, angle.value, value.value))
const valid = computed(() => {
  const field = input.value
  if (field === undefined) return true
  if (value.value === null) return false
  const length = field.signed ? Math.abs(value.value) : value.value
  return length >= field.min && length <= field.max
})
const material = computed(() => !['teach', 'measure', 'check'].includes(props.target.kind))
const outside = computed(() => ['corner', 'boss', 'ball'].includes(props.target.kind))
const travel = computed(() => material.value ? props.limits?.material_travel_mm : props.limits?.travel_mm)
const reduced = window.matchMedia('(prefers-reduced-motion: reduce)').matches
const playing = ref(!reduced)
const active = ref(0)
const stage = useTemplateRef<{ seek: (index: number, settle: boolean) => void }>('stage')
const graphicsAvailable = ref(true)
const step = computed(() => plan.value.steps[active.value] ?? plan.value.steps[0])
watch(plan, () => { active.value = 0 })
function select(index: number): void {
  playing.value = false
  active.value = index
  stage.value?.seek(index, true)
}
function unavailable(): void {
  graphicsAvailable.value = false
  playing.value = false
}
function start(): void {
  if (valid.value) emit('start', value.value ?? 0, angle.value)
}
</script>

<template>
  <ModalDialog :title="plan.title" size="large" expanded fresh-gesture @close="emit('close')">
    <div class="guide-grid">
      <div class="guide-visual">
        <GuideCanvas ref="stage" :plan="plan" :playing="playing" :ruby="material || ruby" @step="active = $event" @unavailable="unavailable" />
        <div class="timeline">
          <span class="phase-count mono">{{ active + 1 }} / {{ plan.steps.length }}</span>
          <span class="phase-name">{{ step?.label }}</span>
          <button class="btn" type="button" aria-label="Previous move" :disabled="active === 0" @click="select(active - 1)">‹</button>
          <button v-if="graphicsAvailable" class="btn play" type="button" @click="playing = !playing">{{ playing ? 'Pause' : 'Play' }}</button>
          <button class="btn" type="button" aria-label="Next move" :disabled="active + 1 >= plan.steps.length" @click="select(active + 1)">›</button>
        </div>
      </div>
      <section class="guide-instructions" aria-label="Probe setup">
        <div class="instruction">
          <h3>Jog here first</h3>
          <p>{{ plan.setup }}</p>
        </div>
        <div v-if="target.kind === 'angle'" class="edge-choice" role="group" aria-label="Edge to measure">
          <button
            v-for="choice in angleChoices" :key="choice.travel" class="btn" type="button"
            :aria-pressed="angle === choice.travel" @click="angle = choice.travel"
          >
            {{ choice.label }}
          </button>
        </div>
        <label v-if="input" class="dimension">
          <span>{{ input.label }} (mm)</span>
          <input
            v-model="text" v-numeric type="text" class="field mono" inputmode="decimal" autocomplete="off"
            data-numeric-unit="mm" data-numeric-action="Set value" :aria-label="input.label"
          />
          <small>{{ input.min }}–{{ input.max }} mm{{ input.signed ? ' in either direction' : '' }}</small>
        </label>
        <dl v-if="travel || limits" class="limits">
          <template v-if="travel"><dt>Search travel</dt><dd>{{ travel }} mm max</dd></template>
          <template v-if="limits"><dt>Search feed</dt><dd>{{ limits.feed_mm_min }} mm/min</dd></template>
          <template v-if="limits?.latch_feed_mm_min">
            <dt>Latch feed</dt><dd>{{ limits.latch_feed_mm_min }} mm/min</dd>
            <dt>Latch backoff</dt><dd>{{ limits.latch_backoff_mm }} mm</dd>
          </template>
          <template v-if="!material && limits"><dt>Setter XY tolerance</dt><dd>{{ limits.xy_tolerance_mm }} mm</dd></template>
          <template v-if="!material && limits?.clearance_mm"><dt>Retract clearance</dt><dd>{{ limits.clearance_mm }} mm</dd></template>
          <template v-if="target.kind === 'check' && limits?.check_tolerance_mm"><dt>Length tolerance</dt><dd>±{{ limits.check_tolerance_mm }} mm</dd></template>
          <template v-if="outside"><dt>Traverse clearance</dt><dd>{{ limits?.material_geometry?.clearance_mm ?? 5 }} mm above top min</dd></template>
          <template v-if="outside"><dt>Ball centre at contact</dt><dd>{{ limits?.material_geometry?.depth_mm ?? 2 }} mm below top</dd></template>
        </dl>
        <div class="guide-result"><h3>Result</h3><p>{{ plan.result }}<template v-if="tool && !material"> · T{{ tool }}</template></p></div>
      </section>
    </div>
    <template #footer>
      <button class="btn" type="button" @click="emit('close')">Cancel</button>
      <button class="btn primary start-probe" type="button" :disabled="!valid" @click="start">{{ target.kind === 'check' ? 'Start check' : 'Start probe' }}</button>
    </template>
  </ModalDialog>
</template>

<style scoped>
.guide-grid { display: grid; grid-template-columns: minmax(0, 1.55fr) minmax(0, 1fr); gap: 16px; min-height: 0; }
.guide-visual { display: grid; grid-template-rows: minmax(205px, 1fr) auto; min-width: 0; min-height: 350px; border: 1px solid var(--line); border-radius: 10px; overflow: hidden; }
.timeline { display: flex; align-items: center; gap: 6px; padding: 6px; border-top: 1px solid var(--line); background: var(--panel); }
.phase-count { padding-left: 4px; white-space: nowrap; color: var(--accent); font-size: 11px; }
.phase-name { flex: 1; min-width: 0; font-size: 12px; line-height: 1.2; }
.timeline button { flex: none; min-height: 44px; min-width: 40px; padding-inline: 7px; }
.timeline button.play { width: 58px; }
.guide-instructions { display: flex; flex-direction: column; gap: 11px; min-width: 0; }
h3 { margin: 0 0 6px; font-size: 13px; font-weight: 650; }
p { margin: 0; line-height: 1.42; font-size: 13px; }
.edge-choice { display: grid; grid-template-columns: 1fr 1fr; gap: 6px; }
.edge-choice button { min-height: 44px; padding-inline: 5px; }
.edge-choice button[aria-pressed=true] { border-color: var(--accent); background: var(--accent-soft); }
.dimension { display: grid; grid-template-columns: auto 92px; gap: 3px 8px; align-items: center; font-size: 12px; }
.dimension input { height: 44px; min-width: 0; padding-inline: 8px; }
.dimension small { grid-column: 1 / -1; color: var(--text-dim); }
.limits { display: grid; grid-template-columns: auto auto; gap: 5px 8px; margin: 0; font-size: 12px; }
.limits dt { color: var(--text-dim); }
.limits dd { margin: 0; text-align: right; white-space: nowrap; }
.guide-result { margin-top: auto; border-top: 1px solid var(--line); padding-top: 9px; }
.start-probe { min-width: 132px; min-height: 44px; }
@media (max-width: 730px) {
  .guide-grid { grid-template-columns: 1fr; }
}
@media (max-height: 820px) {
  .guide-visual { min-height: 325px; }
  .guide-instructions { gap: 7px; }
  h3 { margin-bottom: 4px; }
  .limits { row-gap: 3px; }
}
</style>
