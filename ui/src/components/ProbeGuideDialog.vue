<script setup lang="ts">
import { computed, defineAsyncComponent, ref, useTemplateRef, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { translateMessage } from '../i18n/messages'
import type { FixedSetterLimits } from '../protocol'
import { angleChoiceLabel, angleChoices, guideInput, probeGuide, type AngleEdge, type ProbeGuideTarget } from '../probe-guide'
import { parseNumericExpression } from '../numeric-expression'
import { vNumeric } from '../numeric-input'
import ModalDialog from './ModalDialog.vue'

const GuideCanvas = defineAsyncComponent(() => import('./ProbeGuideCanvas.vue'))
const props = defineProps<{
  target: ProbeGuideTarget
  limits: FixedSetterLimits | undefined
  tool?: number
  ruby?: boolean
  startBlocked?: string | null
}>()
const emit = defineEmits<{ start: [value: number, angle: AngleEdge]; close: [] }>()
const { t } = useI18n()
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
const outside = computed(() => ['corner', 'boss', 'ball', 'ridge'].includes(props.target.kind))
const travel = computed(() => material.value ? props.limits?.material_travel_mm : props.limits?.travel_mm)
const reduced = window.matchMedia('(prefers-reduced-motion: reduce)').matches // i18n-ignore
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
  if (valid.value && !props.startBlocked) emit('start', value.value ?? 0, angle.value)
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
          <button class="btn" type="button" :aria-label="t('probe.guideDialog.previous')" :disabled="active === 0" @click="select(active - 1)">‹</button>
          <button v-if="graphicsAvailable" class="btn play" type="button" @click="playing = !playing">{{ playing ? t('probe.guideDialog.pause') : t('probe.guideDialog.play') }}</button>
          <button class="btn" type="button" :aria-label="t('probe.guideDialog.next')" :disabled="active + 1 >= plan.steps.length" @click="select(active + 1)">›</button>
        </div>
      </div>
      <section class="guide-instructions" :aria-label="t('probe.guideDialog.setup')">
        <div class="instruction">
          <h3>{{ t('probe.guideDialog.jogFirst') }}</h3>
          <p>{{ plan.setup }}</p>
        </div>
        <div v-if="target.kind === 'angle'" class="edge-choice" role="group" :aria-label="t('probe.guideDialog.edgeChoice')">
          <button
            v-for="choice in angleChoices" :key="choice.travel" class="btn" type="button"
            :aria-pressed="angle === choice.travel" @click="angle = choice.travel"
          >
            {{ angleChoiceLabel(choice.travel) }}
          </button>
        </div>
        <label v-if="input" class="dimension">
          <span>{{ t('probe.guideDialog.inputLabel', { label: input.label }) }}</span>
          <input
            v-model="text" v-numeric type="text" class="field mono" inputmode="decimal" autocomplete="off"
            data-numeric-unit="mm" :data-numeric-action="t('probe.guideDialog.setValue')" :aria-label="input.label"
          />
          <small>{{ input.signed ? t('probe.guideDialog.rangeSigned', { min: input.min, max: input.max }) : t('probe.guideDialog.range', { min: input.min, max: input.max }) }}</small>
        </label>
        <dl v-if="travel || limits" class="limits">
          <template v-if="travel"><dt>{{ t('probe.guideDialog.searchTravel') }}</dt><dd>{{ t('probe.guideDialog.travelMax', { value: travel }) }}</dd></template>
          <template v-if="limits"><dt>{{ t('probe.guideDialog.searchFeed') }}</dt><dd>{{ t('probe.guideDialog.feedValue', { value: limits.feed_mm_min }) }}</dd></template>
          <template v-if="limits?.latch_feed_mm_min">
            <dt>{{ t('probe.guideDialog.latchFeed') }}</dt><dd>{{ t('probe.guideDialog.feedValue', { value: limits.latch_feed_mm_min }) }}</dd>
            <dt>{{ t('probe.guideDialog.latchBackoff') }}</dt><dd>{{ limits.latch_backoff_mm }} mm</dd>
          </template>
          <template v-if="!material && limits"><dt>{{ t('probe.guideDialog.setterTolerance') }}</dt><dd>{{ limits.xy_tolerance_mm }} mm</dd></template>
          <template v-if="!material && limits?.clearance_mm"><dt>{{ t('probe.guideDialog.retractClearance') }}</dt><dd>{{ limits.clearance_mm }} mm</dd></template>
          <template v-if="outside"><dt>{{ t('probe.guideDialog.traverseClearance') }}</dt><dd>{{ t('probe.guideDialog.aboveTop', { value: limits?.material_geometry?.clearance_mm ?? 5 }) }}</dd></template>
          <template v-if="outside"><dt>{{ t('probe.guideDialog.ballCentre') }}</dt><dd>{{ t('probe.guideDialog.belowTop', { value: limits?.material_geometry?.depth_mm ?? 2 }) }}</dd></template>
        </dl>
        <div class="guide-result">
          <h3>{{ t('probe.guideDialog.result') }}<span v-if="tool && !material" class="result-tool mono">T{{ tool }}</span></h3>
          <p>{{ plan.result }}</p>
          <p v-if="target.kind === 'check' && limits?.check_tolerance_mm" class="check-tolerance">
            {{ t('probe.guideDialog.lengthTolerance') }} <strong class="mono">±{{ limits.check_tolerance_mm }} mm</strong>
          </p>
        </div>
      </section>
    </div>
    <template #footer>
      <p v-if="startBlocked" class="start-block" role="status">{{ translateMessage(startBlocked) }}</p>
      <button class="btn" type="button" @click="emit('close')">{{ t('common.cancel') }}</button>
      <button class="btn primary start-probe" type="button" :disabled="!valid || !!startBlocked" @click="start">{{ target.kind === 'check' ? t('probe.guideDialog.startCheck') : t('probe.guideDialog.startProbe') }}</button>
    </template>
  </ModalDialog>
</template>

<style scoped>
.guide-grid { display: grid; flex: 1; grid-template-columns: minmax(0, 1.55fr) minmax(0, 1fr); gap: 16px; min-height: 0; }
.guide-visual { display: grid; grid-template-rows: minmax(205px, 1fr) auto; min-width: 0; min-height: 350px; border: 1px solid var(--line); border-radius: 10px; overflow: hidden; }
.timeline { display: flex; align-items: center; gap: 6px; padding: 6px; border-top: 1px solid var(--line); background: var(--panel); }
.phase-count { padding-left: 4px; white-space: nowrap; color: var(--accent); font-size: 11px; }
.phase-name { flex: 1; min-width: 0; font-size: 12px; line-height: 1.2; }
.timeline button { flex: none; min-height: 44px; min-width: 44px; padding-inline: 7px; }
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
.guide-result h3 { display: flex; align-items: baseline; justify-content: space-between; gap: 8px; }
.result-tool { color: var(--text-dim); font-size: 12px; }
.check-tolerance { display: flex; align-items: baseline; justify-content: space-between; gap: 8px; margin-top: 6px; color: var(--text-dim); }
.check-tolerance strong { color: var(--text); white-space: nowrap; }
.start-block { align-self: center; margin-right: auto; color: var(--warn); }
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
