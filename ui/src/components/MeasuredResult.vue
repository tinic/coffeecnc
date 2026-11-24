<script setup lang="ts">
// The card of a measure-only probe (#247): what it found, in work and machine coordinates, to
// copy, and to set as the work origin after a review. Nothing here moves the machine.
import { computed, shallowRef } from 'vue'
import { useI18n } from 'vue-i18n'
import { panel } from '../machine'
import type { ProbeDirection, SetterMeasured, SetterState, Vec3 } from '../protocol'
import { formatLength, wcsName } from '../units'
import ModalDialog from './ModalDialog.vue'

const props = defineProps<{ result: SetterState; measured: SetterMeasured; canApply: boolean; stale: boolean }>()
const emit = defineEmits<{ apply: []; close: [] }>()
const { t } = useI18n()
const reviewing = shallowRef(false)
const copyNote = shallowRef<string | null>(null)

const unit = computed(() => panel.unit === 'inch' ? 'in' : 'mm')
const length = (mm: number): string => formatLength(mm, panel.unit)
const axes = ['x', 'y', 'z'] as const
const triple = (point: Vec3): string => point.map((value, index) => `${'XYZ'[index] ?? ''} ${length(value)}`).join('  ')
const travel: Record<ProbeDirection, string> = { z_top: '−Z', x_minus: '−X', x_plus: '+X', y_minus: '−Y', y_plus: '+Y' }
const wcs = computed(() => wcsName(props.measured.wcs))
const axesText = computed(() => props.measured.axes.split('').map((axis) => `${axis}0`).join(' '))
const work = computed(() => axes.flatMap((axis) => {
  const value = props.measured.work[axis]
  return value === undefined ? [] : [`${axis.toUpperCase()} ${length(value)}`]
}).join('  '))
const size = computed(() => {
  const { x, y } = props.result.feature_size ?? {}
  return [x === undefined ? null : `X ${length(x)}`, y === undefined ? null : `Y ${length(y)}`].filter(Boolean).join(' × ')
})
const angle = computed(() => {
  const degrees = props.result.edge_angle
  return degrees === undefined ? null : `${degrees >= 0 ? '+' : '−'}${Math.abs(degrees).toFixed(3)}°`
})

/** The card as plain text, one value per line. */
function text(): string {
  const lines: string[] = []
  if (work.value) lines.push(`${t('probe.materialProbe.resultWork', { wcs: wcs.value })}: ${work.value} ${unit.value}`)
  if (props.measured.machine) lines.push(`${t('probe.materialProbe.resultMachine')}: ${triple(props.measured.machine)} ${unit.value}`)
  if (props.result.center_diameter !== undefined)
    lines.push(`${t('probe.materialProbe.resultDiameter')}: ${length(props.result.center_diameter)} ${unit.value}`)
  if (props.result.center_uncertainty !== undefined)
    lines.push(`${t('probe.materialProbe.resultUncertainty')}: ±${length(props.result.center_uncertainty)} ${unit.value}`)
  if (size.value) lines.push(`${t('probe.materialProbe.resultSize')}: ${size.value} ${unit.value}`)
  if (angle.value) lines.push(`${t('probe.materialProbe.resultAngle')}: ${angle.value}`)
  for (const face of props.measured.faces) {
    lines.push(`${travel[face.direction]}  ${t('probe.materialProbe.resultWork', { wcs: wcs.value })}: ${triple(face.work)}  ` +
      `${t('probe.materialProbe.resultMachine')}: ${triple(face.machine)} ${unit.value}`)
  }
  return lines.join('\n')
}
async function copy(): Promise<void> {
  try {
    await navigator.clipboard.writeText(text())
    copyNote.value = t('probe.materialProbe.copied')
  } catch {
    copyNote.value = t('probe.materialProbe.copyFailed')
  }
}
</script>

<template>
  <ModalDialog
    :title="reviewing ? t('probe.materialProbe.applyMeasuredTitle', { wcs, axes: axesText }) : t('probe.materialProbe.resultTitle')"
    size="medium" fresh-gesture @close="emit('close')"
  >
    <template v-if="!reviewing">
      <p class="note">{{ t('probe.materialProbe.resultNothingSet') }}</p>
      <dl class="values mono">
        <template v-if="work">
          <dt>{{ t('probe.materialProbe.resultPoint') }} · {{ t('probe.materialProbe.resultWork', { wcs }) }}</dt>
          <dd>{{ work }} {{ unit }}</dd>
        </template>
        <template v-if="measured.machine">
          <dt>{{ t('probe.materialProbe.resultPoint') }} · {{ t('probe.materialProbe.resultMachine') }}</dt>
          <dd>{{ triple(measured.machine) }} {{ unit }}</dd>
        </template>
        <template v-if="result.center_diameter !== undefined">
          <dt>{{ t('probe.materialProbe.resultDiameter') }}</dt><dd>{{ length(result.center_diameter) }} {{ unit }}</dd>
        </template>
        <template v-if="result.center_uncertainty !== undefined">
          <dt>{{ t('probe.materialProbe.resultUncertainty') }}</dt><dd>±{{ length(result.center_uncertainty) }} {{ unit }}</dd>
        </template>
        <template v-if="size"><dt>{{ t('probe.materialProbe.resultSize') }}</dt><dd>{{ size }} {{ unit }}</dd></template>
        <template v-if="angle"><dt>{{ t('probe.materialProbe.resultAngle') }}</dt><dd>{{ angle }}</dd></template>
      </dl>
      <h3>{{ t('probe.materialProbe.resultFaces') }}</h3>
      <ul class="faces mono">
        <li v-for="(face, index) in measured.faces" :key="index">
          <strong>{{ travel[face.direction] }}</strong>
          <span>{{ t('probe.materialProbe.resultWork', { wcs }) }} {{ triple(face.work) }}</span>
          <span>{{ t('probe.materialProbe.resultMachine') }} {{ triple(face.machine) }}</span>
        </li>
      </ul>
      <p v-if="stale && measured.measurement_id !== undefined" class="warn">{{ t('probe.materialProbe.measuredStale') }}</p>
      <p v-if="copyNote" class="note" role="status">{{ copyNote }}</p>
    </template>
    <p v-else class="effect">{{ t('probe.materialProbe.applyMeasuredEffect', { values: work, axes: axesText }) }}</p>
    <template #footer>
      <template v-if="!reviewing">
        <button class="btn" type="button" @click="copy">{{ t('probe.materialProbe.copy') }}</button>
        <button
          v-if="measured.measurement_id !== undefined" class="btn primary" type="button" :disabled="!canApply"
          @click="reviewing = true"
        >
          {{ t('probe.materialProbe.applyMeasured', { axes: axesText }) }}
        </button>
      </template>
      <template v-else>
        <button class="btn" type="button" @click="reviewing = false">{{ t('common.cancel') }}</button>
        <button class="btn primary" type="button" :disabled="!canApply" @click="emit('apply')">
          {{ t('probe.materialProbe.applyMeasuredConfirm', { axes: axesText }) }}
        </button>
      </template>
    </template>
  </ModalDialog>
</template>

<style scoped>
p, h3 { margin: 0; line-height: 1.45; }
h3 { margin-top: 10px; font-size: 13px; font-weight: 650; }
.note { color: var(--text-dim); font-size: 12px; }
.warn { margin-top: 8px; color: var(--warn); }
.effect { line-height: 1.5; }
.values { display: grid; grid-template-columns: auto 1fr; gap: 4px 12px; margin: 8px 0 0; font-size: 12px; }
.values dt { color: var(--text-dim); }
.values dd { margin: 0; text-align: right; user-select: text; }
.faces { display: grid; gap: 4px; margin: 4px 0 0; padding: 0; list-style: none; font-size: 11px; user-select: text; }
.faces li { display: grid; grid-template-columns: 28px 1fr 1fr; gap: 8px; }
footer .btn, .btn { min-height: 44px; }
</style>
