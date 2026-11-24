<script setup lang="ts">
import { vTooltip } from '../tooltip'
// The native offset history, newest first: every change of a work offset and of a tool's
// length or diameter, with who made it. Restore reviews the values it writes back first.
import { computed, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'

import { translateMessage } from '../i18n/messages'
import { findTool, mdiBlock, panel, send } from '../machine'
import { entryTime, fieldAxis, fieldName, fieldValue, matchesFilter, restoreBlock, sourceName, targetName, type HistoryFilter } from '../offset-history'
import type { HistoryEntry, HistoryField } from '../protocol'
import { workOffsetRows } from '../work-offsets'
import ModalDialog from './ModalDialog.vue'

const props = withDefaults(defineProps<{ filter?: HistoryFilter }>(), { filter: 'all' })
const emit = defineEmits<{ close: [] }>()
const { t } = useI18n()

const unit = computed(() => panel.unit)
const unitName = computed(() => unit.value === 'inch' ? 'in' : 'mm')
const filter = ref<HistoryFilter>(props.filter)
const filters = computed((): readonly (readonly [HistoryFilter, string])[] => [
  ['all', t('probe.history.all')], ['offsets', t('probe.history.offsets')], ['tools', t('probe.history.tools')],
])
const blocked = computed(() => mdiBlock.value === null ? null : translateMessage(mdiBlock.value))
const history = computed(() => panel.offsetHistory)
const entries = computed(() => history.value?.entries.filter((entry) => matchesFilter(entry, filter.value)) ?? [])
const storage = computed(() => {
  const shown = history.value
  if (shown === null) return null
  const reason = shown.reason ? translateMessage(shown.reason, shown.localization) : null
  if (shown.storage === 'session_only') return reason
    ? t('probe.history.sessionOnlyReason', { reason }) : t('probe.history.sessionOnly')
  return reason
})
const now = Date.now()
/** When the change a restore wrote back was made, while it is still in the history. */
function restoredAt(entry: HistoryEntry): string | null {
  const restored = entry.restores === null ? undefined : history.value?.entries.find((candidate) => candidate.id === entry.restores)
  return restored === undefined ? null : entryTime(restored.time, now)
}

/** The value the field has now, as far as the panel is told. */
function current(entry: HistoryEntry, field: HistoryField): number | null {
  const axis = fieldAxis(field)
  if (entry.target === 'tool') {
    const tool = findTool(entry.number)
    return tool === null ? null : field === 'length' ? tool.length : field === 'diameter' ? tool.diameter : null
  }
  if (entry.target === 'g92') return panel.status?.link === 'live' && axis !== null ? panel.status.offsets.g92[axis] : null
  const row = workOffsetRows(panel.workOffsets, panel.status, panel.link).find((candidate) => candidate.wcs === entry.number)
  if (row === undefined) return null
  return field === 'rotation_xy' ? row.rotation_xy : axis !== null ? row.offset?.[axis] ?? null : null
}

const reviewing = ref<HistoryEntry | null>(null)
const reviewChanges = computed(() => {
  const entry = reviewing.value
  return entry === null ? [] : entry.changes.map((change) => ({ ...change, current: current(entry, change.field) }))
})
const reviewBlock = computed(() => {
  const entry = reviewing.value
  if (entry === null) return null
  if (!history.value?.entries.some((candidate) => candidate.id === entry.id)) return t('probe.history.gone')
  return blocked.value ?? restoreBlock(entry)
})
const reviewTitle = computed(() => {
  const entry = reviewing.value
  if (entry === null) return ''
  return t('probe.history.reviewTitle', { target: targetName(entry), fields: entry.changes.map((change) => fieldName(change.field)).join(', ') })
})
const reviewEffect = computed(() => {
  const entry = reviewing.value
  if (entry === null) return ''
  if (entry.target === 'tool') return t('probe.history.effectTool', { target: targetName(entry) })
  if (entry.target === 'g92') return t('probe.history.effectShift')
  return t('probe.history.effectWork', { target: targetName(entry) })
})
function review(entry: HistoryEntry): void {
  if (restoreBlock(entry) === null && mdiBlock.value === null) reviewing.value = entry
}
function confirm(): void {
  const entry = reviewing.value
  if (entry === null || reviewBlock.value !== null) return
  send({ type: 'restore_offset', entry: entry.id })
  reviewing.value = null
}
watch((): unknown => panel.hello, () => { reviewing.value = null })
</script>

<template>
  <ModalDialog v-if="reviewing !== null" key="review" :title="reviewTitle" size="medium" expanded fresh-gesture @close="reviewing = null">
    <table class="changes">
      <thead><tr><th></th><th>{{ t('probe.history.now') }}</th><th>{{ t('probe.history.restored') }}</th></tr></thead>
      <tbody>
        <tr v-for="change in reviewChanges" :key="change.field">
          <th>{{ fieldName(change.field) }}</th>
          <td class="mono">{{ change.current === null ? t('common.unavailable') : fieldValue(change.field, change.current, unit) }}<span v-if="change.field !== 'rotation_xy' && change.current !== null" class="value-unit">{{ ' ' + unitName }}</span></td>
          <td class="mono">{{ fieldValue(change.field, change.before, unit) }}<span v-if="change.field !== 'rotation_xy'" class="value-unit">{{ ' ' + unitName }}</span></td>
        </tr>
      </tbody>
    </table>
    <p>{{ reviewEffect }} {{ t('probe.history.noMotion') }}</p>
    <p class="hint">{{ t('probe.history.changedBy', { source: sourceName(reviewing.source), time: entryTime(reviewing.time, now) }) }}</p>
    <p v-if="reviewBlock" class="blocked" role="status">{{ reviewBlock }}</p>
    <template #footer>
      <button class="btn" type="button" @click="reviewing = null">{{ t('common.back') }}</button>
      <button class="btn primary" type="button" :disabled="reviewBlock !== null" @click="confirm">{{ t('probe.history.restoreTarget', { target: targetName(reviewing) }) }}</button>
    </template>
  </ModalDialog>
  <ModalDialog v-else key="list" :title="t('probe.history.title')" size="large" expanded contained @close="emit('close')">
    <div class="bar">
      <div class="segmented" role="group" :aria-label="t('probe.history.show')">
        <button v-for="[value, label] in filters" :key="value" class="btn" type="button" :class="{ on: filter === value }" :aria-pressed="filter === value" @click="filter = value">{{ label }}</button>
      </div>
      <span v-if="storage" class="hint storage">{{ storage }}</span>
    </div>
    <p v-if="history === null" class="hint">{{ t('probe.history.noHistory') }}</p>
    <p v-else-if="entries.length === 0" class="hint">{{ t('probe.history.empty') }}</p>
    <ul v-else class="entries" :aria-label="t('probe.history.list')">
      <li v-for="entry in entries" :key="entry.id" class="entry">
        <span class="when mono">{{ entryTime(entry.time, now) }}</span>
        <span class="what">
          <strong class="mono">{{ targetName(entry) }}</strong>
          <span class="source" :class="{ outside: entry.source === 'outside' }">{{ restoredAt(entry) === null ? sourceName(entry.source) : t('probe.history.sourceOf', { source: sourceName(entry.source), time: restoredAt(entry) ?? '' }) }}</span>
        </span>
        <span class="values">
          <span v-for="change in entry.changes" :key="change.field" class="change mono">
            <span class="axis-name">{{ fieldName(change.field) }}</span>
            {{ fieldValue(change.field, change.before, unit) }} → {{ fieldValue(change.field, change.after, unit) }}
          </span>
        </span>
        <button v-tooltip="restoreBlock(entry) ?? blocked ?? t('probe.history.restoreTip', { target: targetName(entry) })" class="btn restore" type="button" :disabled="restoreBlock(entry) !== null || mdiBlock !== null" :aria-label="t('probe.history.restoreLabel', { target: targetName(entry), time: entryTime(entry.time, now) })" @click="review(entry)">{{ t('probe.history.restore') }}</button>
      </li>
    </ul>
    <template #footer>
      <span class="hint footer-note">{{ t('probe.history.units', { unit: unitName }) }}</span>
      <button class="btn" type="button" @click="emit('close')">{{ t('common.close') }}</button>
    </template>
  </ModalDialog>
</template>

<style scoped>
p { margin: 0; line-height: 1.45; }
.bar { display: flex; flex: none; align-items: center; gap: 12px; }
.bar .btn { min-height: 44px; padding: 0 14px; font-size: 13px; }
.storage { margin-left: auto; color: var(--warn); font-size: 12px; text-align: right; }
.entries { display: flex; flex: 1; flex-direction: column; min-height: 0; margin: 0; padding: 0; overflow: auto; list-style: none; }
.entry { flex: none; display: grid; grid-template-columns: 9ch minmax(140px, 0.9fr) minmax(0, 1.6fr) auto; align-items: center; gap: 12px; min-height: 56px; padding: 6px 4px; border-bottom: 1px solid var(--line); }
.when { font-size: 13px; color: var(--text-dim); }
.what { display: flex; flex-direction: column; gap: 2px; min-width: 0; }
.what strong { font-size: 15px; }
.source { font-size: 12px; color: var(--text-dim); }
.source.outside { color: var(--warn); }
.values { display: flex; flex-wrap: wrap; gap: 4px 16px; font-size: 14px; }
.change { white-space: nowrap; }
.axis-name { margin-right: 6px; color: var(--text-dim); font-family: inherit; font-size: 12px; }
.restore { min-height: 44px; padding: 0 12px; font-size: 13px; }
.footer-note { margin-right: auto; align-self: center; font-size: 12px; }
.changes { width: 100%; border-collapse: collapse; }
.changes th, .changes td { padding: 10px 8px; border-bottom: 1px solid var(--line); text-align: right; }
.changes th:first-child { text-align: left; }
.changes thead { color: var(--text-dim); font-size: 12px; }
.changes td { font-size: 19px; white-space: nowrap; }
.value-unit { color: var(--text-dim); font-size: 12px; }
.btn { min-height: 44px; }
</style>
