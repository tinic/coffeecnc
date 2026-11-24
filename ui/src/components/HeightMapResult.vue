<script setup lang="ts">
// A Z height map's card (#247): the heights as a grid, saved as CSV and as LinuxCNC's probe file
// into the program folder, and shown in the 3D view until hidden.
import { computed, onBeforeUnmount, shallowRef } from 'vue'
import { useI18n } from 'vue-i18n'
import { translateMessage } from '../i18n/messages'
import { onResult, panel, send } from '../machine'
import type { CommandBody, HeightMap } from '../protocol'
import { heightMap, heightMapCsv, heightMapName, heightMapProbeFile, heightMapSidecar, heightRange, measuredPoints } from '../height-map'
import { formatLength, wcsName } from '../units'
import ModalDialog from './ModalDialog.vue'

const props = defineProps<{ map: HeightMap }>()
const emit = defineEmits<{ close: [] }>()
const { t } = useI18n()
const unit = computed(() => panel.unit === 'inch' ? 'in' : 'mm')
const length = (mm: number): string => formatLength(mm, panel.unit)
const range = computed(() => heightRange(props.map))
/** Rows from the far Y down, as the map lies seen from above; null where a partial map stopped short. */
const rows = computed(() => Array.from({ length: props.map.rows }, (_, row) => props.map.rows - 1 - row).map((row) =>
  Array.from({ length: props.map.columns }, (_, column) => props.map.points[row * props.map.columns + column]?.work[2] ?? null)))
const measured = computed(() => measuredPoints(props.map).length)
const shown = computed(() => heightMap.value === props.map)
function toggle(): void {
  heightMap.value = shown.value ? null : props.map
}

const note = shallowRef<string | null>(null)
let saving: CommandBody[] = []
function write(path: string, text: string): void {
  const request: CommandBody = { type: 'save_program', path, text }
  saving.push(request)
  note.value = t('probe.heightMap.saving', { name: path })
  send(request)
}
/** The CSV with its frame in a comment; the probe file pure, its frame in a .txt.json beside it. */
function save(kind: 'csv' | 'probe'): void {
  const at = new Date()
  const name = heightMapName(at, props.map.partial !== undefined)
  if (kind === 'csv') {
    write(`${name}.csv`, heightMapCsv(props.map, panel.unit, at)) // i18n-ignore: a file name
    return
  }
  write(`${name}.txt.json`, heightMapSidecar(props.map, panel.unit, at)) // i18n-ignore: a file name
  write(`${name}.txt`, heightMapProbeFile(props.map, panel.unit)) // i18n-ignore: a file name
}
const removeListener = onResult((command, result) => {
  if (!saving.includes(command) || command.type !== 'save_program') return
  saving = saving.filter((entry) => entry !== command)
  note.value = result.outcome === 'accepted' ? t('probe.heightMap.saved', { name: command.path })
    : translateMessage(result.reason || t('probe.heightMap.notSaved'), result.refusal)
})
onBeforeUnmount(removeListener)
</script>

<template>
  <ModalDialog :title="t('probe.heightMap.resultTitle')" size="medium" fresh-gesture @close="emit('close')">
    <p class="summary">
      {{ t('probe.heightMap.summary', { wcs: wcsName(map.wcs), count: measured, low: length(range.low), high: length(range.high), spread: length(range.high - range.low), unit }) }}
    </p>
    <p v-if="map.partial !== undefined" class="partial" role="status">
      {{ t('probe.heightMap.partialSummary', { count: measured, total: map.points.length, reason: translateMessage(map.partial, map.partial_localization) }) }}
    </p>
    <table class="heights mono" :aria-label="t('probe.heightMap.heights')">
      <tbody>
        <tr v-for="(row, index) in rows" :key="index">
          <td v-for="(z, column) in row" :key="column">{{ z === null ? '–' : length(z) }}</td>
        </tr>
      </tbody>
    </table>
    <p v-if="note" class="note" role="status">{{ note }}</p>
    <template #footer>
      <button class="btn" type="button" @click="toggle">{{ shown ? t('probe.heightMap.hide') : t('probe.heightMap.show') }}</button>
      <button class="btn" type="button" @click="save('csv')">{{ t('probe.heightMap.saveCsv') }}</button>
      <button class="btn" type="button" @click="save('probe')">{{ t('probe.heightMap.saveProbe') }}</button>
    </template>
  </ModalDialog>
</template>

<style scoped>
p { margin: 0; line-height: 1.45; font-size: 13px; }
.heights { margin: 10px 0; border-collapse: collapse; font-size: 11px; }
.heights td { padding: 2px 6px; border: 1px solid var(--line); text-align: right; }
.note { color: var(--text-dim); font-size: 12px; }
.partial { margin-top: 6px; color: var(--warn); }
footer .btn { min-height: 44px; }
</style>
