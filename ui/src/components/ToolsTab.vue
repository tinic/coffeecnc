<script setup lang="ts">
import { vTooltip } from '../tooltip'
// Tools and offsets: the tool table, the tool in the spindle with its length touch-off, and the
// nine work coordinate systems.
import { computed, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'

import { startDialog } from '../dialogs'
import { findTool, linkBlock, mdiBlock, panel, send } from '../machine'
import type { Tool } from '../protocol'
import { formatLength, fromDisplay, toDisplay, toolLabel, wcsName } from '../units'
import { workOffsetRows, type WorkOffsetRow } from '../work-offsets'
import AppIcon from './AppIcon.vue'
import FixedSetterControls from './FixedSetterControls.vue'
import SetterStatusBand from './SetterStatusBand.vue'
import ModalDialog from './ModalDialog.vue'
import NumberDialog from './NumberDialog.vue'
import ToolEditor from './ToolEditor.vue'
import ToolPreviewDialog from './ToolPreviewDialog.vue'
import WorkOffsetDialog from './WorkOffsetDialog.vue'
import OffsetHistoryDialog from './OffsetHistoryDialog.vue'
import type { HistoryFilter } from '../offset-history'
import { setterStatus, touchedOff } from '../setter-state'
import { defaultToolName } from '../tool-models'

const { t } = useI18n()
const shiftKey = 'tools.tab.shift'

const status = computed(() => panel.status)
const unit = computed(() => panel.unit)
const tools = computed(() => panel.toolTable?.tools
  .filter((tool) => tool.number > 0)
  .sort((a, b) => a.number - b.number) ?? [])
const selectedTool = ref<number | null>(null)
// A tap selects a row; a second tap on it within 400 ms opens the tool preview (not the editor).
const previewTool = ref<Tool | null>(null)
let lastTap: { number: number; at: number } | null = null
function tap(tool: Tool): void {
  const now = Date.now()
  if (lastTap?.number === tool.number && now - lastTap.at < 400) { previewTool.value = tool; lastTap = null }
  else lastTap = { number: tool.number, at: now }
  selectedTool.value = tool.number
}
watch(tools, (rows) => {
  if (!rows.some((tool) => tool.number === selectedTool.value)) selectedTool.value = null
})
const loaded = computed(() => status.value?.tool.in_spindle ?? 0)
const loadedTool = computed(() => findTool(loaded.value))
const wcs = computed(() => wcsName(status.value?.offsets.wcs ?? 1))
const applied = computed(() => status.value?.offsets.tool[2] ?? 0)
/** Why the tool table cannot be edited, or null. */
const editBlock = computed((): string | null => {
  if (linkBlock.value !== null || status.value === null) {
    return linkBlock.value ?? t('tools.tab.noStatus')
  }
  return status.value.interp === 'idle' ? null : t('tools.tab.programRunning')
})

const touchBlock = computed((): string | null => {
  if (mdiBlock.value !== null) {
    return mdiBlock.value
  }
  return loaded.value === 0 ? t('tools.tab.noToolInSpindle') : null
})

/** The table length differs from the offset in effect: LinuxCNC applies a length only at G43. */
const lengthMismatch = computed(
  () => loadedTool.value !== null && applied.value !== 0 && Math.abs(loadedTool.value.length - applied.value) > 1e-6,
)

// ---- tool row editor ------------------------------------------------------------------------------

const editing = ref<{ tool: Tool | null } | null>(null)
function sameTool(tool: Tool): boolean {
  const current = findTool(tool.number)
  return current !== null && current.pocket === tool.pocket && current.length === tool.length &&
    current.diameter === tool.diameter && current.comment === tool.comment
}
function edit(tool: Tool): void { if (editBlock.value === null) editing.value = { tool: { ...tool } } }
function add(): void { if (editBlock.value === null) editing.value = { tool: null } }
if (startDialog === 'tool') add()

// ---- touch-off ---------------------------------------------------------------------------------------

const touching = ref<{ initial: string; millimeters: number } | null>(null)
function openTouchOff(): void {
  if (touchBlock.value !== null) return
  const millimeters = panel.preferences.tool_reference_height
  touching.value = { initial: String(Number(toDisplay(millimeters, unit.value).toFixed(6))), millimeters }
}

function touchOff(value: number, text: string): void {
  if (touching.value !== null && touchBlock.value === null) {
    send({ type: 'tool_touch_off', value: text === touching.value.initial ? touching.value.millimeters : fromDisplay(value, unit.value) })
  }
  touching.value = null
}
// Position feedback never replaces the draft; a changed tool, work system or connection does.
watch([(): unknown => panel.hello, loaded, wcs, unit, touchBlock], () => { touching.value = null }, { flush: 'sync' })

const loading = ref<Tool | null>(null)
const loadBlock = computed(() => mdiBlock.value ?? (loading.value !== null && !sameTool(loading.value)
  ? t('tools.tab.tableChanged') : null))
function openLoad(tool: Tool): void {
  if (mdiBlock.value === null) loading.value = { ...tool }
}
function loadTool(): void {
  if (loading.value === null || loadBlock.value !== null) return
  const number = loading.value.number
  loading.value = null
  send({ type: 'mdi', text: `T${String(number)} M6 G43 H${String(number)}` })
}
watch([(): unknown => panel.hello, loaded, mdiBlock], () => { loading.value = null }, { flush: 'sync' })

const systems = computed(() => workOffsetRows(panel.workOffsets, status.value, panel.link))
function offsetHint(system: WorkOffsetRow): string | undefined {
  if (system.offset === null) return t('tools.tab.offsetsUnavailable', { system: wcsName(system.wcs) })
  if (system.stale) return t('tools.tab.offsetsStale', { system: wcsName(system.wcs) })
  if (system.unconfirmed) return t('tools.tab.offsetsUnconfirmed', { system: wcsName(system.wcs) })
  return undefined
}
const rotated = computed(() => systems.value.some((system) => system.rotation_xy !== null && system.rotation_xy !== 0))
const offsetEditor = ref<number | null>(null)
function editOffset(wcs: number): void {
  if (mdiBlock.value === null && panel.link === 'live' && status.value?.link === 'live') offsetEditor.value = wcs
}
const historyFilter = ref<HistoryFilter | null>(startDialog === 'history' ? 'all' : null)
</script>

<template>
  <div class="tools">
    <section class="card table">
      <div class="heading">
        <h2>{{ t('tools.tab.toolTable') }}</h2>
        <span class="hint spacer">{{ unit }}</span>
        <button v-if="!panel.hello?.fixed_setter" v-tooltip="touchBlock ?? t('tools.tab.touchOffLengthHint')" class="btn touch-length" type="button" :disabled="touchBlock !== null" @click="openTouchOff">{{ t('tools.tab.touchOffLength') }}</button>
        <FixedSetterControls v-if="panel.hello?.fixed_setter" :limits="panel.hello.fixed_setter" />
        <button v-tooltip="editBlock ?? t('tools.tab.addToolHint')" class="btn" type="button" :disabled="editBlock !== null" @click="add">
          <AppIcon name="plus" /><span>{{ t('tools.tab.addTool') }}</span>
        </button>
        <button v-if="panel.offsetHistory" v-tooltip="t('tools.tab.toolHistoryHint')" class="btn square tool-history" type="button" :aria-label="t('tools.tab.toolHistory')" @click="historyFilter = 'tools'"><AppIcon name="history" /></button>
      </div>
      <SetterStatusBand v-if="panel.hello?.fixed_setter" :line="setterStatus" />

      <p v-if="lengthMismatch" class="blocked">{{ t('tools.tab.lengthMismatch') }}</p>

      <div class="scroll">
        <table>
          <thead>
            <tr>
              <th class="num tool-column">{{ t('tools.tab.columnTool') }}</th>
              <th v-tooltip="t('tools.tab.columnSetHint')" class="touched-column">{{ t('tools.tab.columnSet') }}</th>
              <th v-tooltip="t('tools.tab.columnLengthHint')" class="num dimension-column">{{ t('tools.tab.columnLength') }}</th>
              <th class="num dimension-column">{{ t('tools.tab.columnDiameter') }}</th>
              <th>{{ t('tools.tab.columnDescription') }}</th>
              <th class="load-column"></th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="tool in tools" :key="tool.number" class="tool-row" :class="{ loaded: tool.number === loaded, selected: tool.number === selectedTool }" @click="tap(tool)">
              <td class="num mono">
                <button class="tool-number" type="button" :aria-label="t('tools.tab.selectTool', { number: tool.number })" :aria-pressed="tool.number === selectedTool" @click.stop="tap(tool)">{{ toolLabel(tool.number) }}</button>
              </td>
              <td class="touched-cell">
                <span v-if="touchedOff(tool.number)" class="touched" :aria-label="t('tools.tab.touchedOff')"><AppIcon name="check" /></span>
              </td>
              <td class="num mono">{{ formatLength(tool.length, unit) }}</td>
              <td class="num mono">{{ formatLength(tool.diameter, unit) }}</td>
              <td class="comment">
                {{ tool.comment || defaultToolName(tool.number) }}
              </td>
              <td class="actions">
                <button v-tooltip="editBlock ?? t('tools.tab.editToolHint', { number: tool.number })" class="btn quiet edit-tool" type="button" :disabled="editBlock !== null" :aria-label="t('tools.tab.editToolLabel', { number: tool.number })" @click.stop="edit(tool)">{{ t('common.edit') }}</button>
                <button v-tooltip="mdiBlock ?? t('tools.tab.loadToolHint', { number: tool.number })" class="btn quiet load-tool" type="button" :disabled="mdiBlock !== null" :aria-label="t('tools.tab.loadToolLabel', { number: tool.number })" @click.stop="openLoad(tool)">{{ t('tools.tab.load') }}</button>
              </td>
            </tr>
          </tbody>
        </table>
        <p v-if="panel.toolTable === null" class="hint">{{ t('tools.tab.noToolTable') }}</p>
      </div>
    </section>

    <section class="card offsets">
      <div class="heading">
        <h2>{{ t('tools.tab.workOffsets') }}</h2>
        <span class="hint">{{ t('tools.tab.workOffsetsHint', { unit }) }}</span>
        <span v-if="systems.some(system => system.stale)" class="offset-history">{{ t('tools.tab.lastReported') }}</span>
        <span v-else-if="systems.some(system => system.unconfirmed)" v-tooltip="t('tools.tab.unconfirmedHint')" class="offset-history unconfirmed-note">{{ t('tools.tab.unconfirmed') }}</span>
        <button v-if="panel.offsetHistory" v-tooltip="t('tools.tab.historyHint')" class="btn history" type="button" @click="historyFilter = 'all'"><AppIcon name="history" /><span>{{ t('tools.tab.history') }}</span></button>
      </div>
      <div class="scroll">
        <table>
          <thead>
            <tr>
              <th>{{ t('tools.tab.columnSystem') }}</th>
              <th class="num">X</th>
              <th class="num">Y</th>
              <th class="num">Z</th>
              <th v-if="rotated" class="num">{{ t('tools.tab.columnRotation') }}</th>
              <th></th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="system in systems" :key="system.wcs" class="offset-row" :class="{ loaded: system.active, unconfirmed: system.unconfirmed, editable: mdiBlock === null && panel.link === 'live' && status?.link === 'live' }" :tabindex="mdiBlock === null && panel.link === 'live' && status?.link === 'live' ? 0 : -1" :aria-label="t('tools.tab.editWorkOrigins', { system: wcsName(system.wcs) })" @click="editOffset(system.wcs)" @keydown.enter.self.prevent="editOffset(system.wcs)" @keydown.space.self.prevent="editOffset(system.wcs)">
              <td v-tooltip="offsetHint(system)" class="mono" :aria-label="offsetHint(system)">{{ wcsName(system.wcs) }}</td>
              <td v-for="(value, axis) in system.offset ?? [null, null, null]" :key="axis" class="num mono">{{ value === null ? '—' : formatLength(value, unit) }}</td>
              <td v-if="rotated" class="num mono">{{ system.rotation_xy === null ? '—' : system.rotation_xy === 0 ? '' : `${system.rotation_xy}°` }}</td>
              <td class="actions">
                <span v-if="system.active" class="chip go">{{ t('tools.tab.active') }}</span>
                <button
                  v-else
                  v-tooltip="mdiBlock ?? t('tools.tab.selectHint', { system: wcsName(system.wcs) })"
                  class="btn quiet select"
                  type="button"
                  :disabled="mdiBlock !== null || status?.link !== 'live'"
                  @click.stop="send({ type: 'select_wcs', wcs: system.wcs })"
                >
                  {{ t('tools.tab.select') }}
                </button>
                <button class="btn quiet select" type="button" :aria-label="t('tools.tab.editOrigins', { system: wcsName(system.wcs) })" :disabled="mdiBlock !== null || panel.link !== 'live' || status?.link !== 'live'" @click.stop="editOffset(system.wcs)">{{ t('common.edit') }}</button>
              </td>
            </tr>
          </tbody>
        </table>
        <p v-if="panel.workOffsets === null" class="hint">{{ t('tools.tab.noWorkOffsets') }}</p>
      </div>
      <i18n-t v-if="status?.offsets.g92.some((v) => v !== 0)" :keypath="shiftKey" tag="p" class="blocked g92" scope="global">
        <template #shift><span class="mono">{{ status.offsets.g92.map((v) => formatLength(v, unit)).join('  ') }}</span></template>
      </i18n-t>
    </section>

    <ToolEditor v-if="editing !== null" :tool="editing.tool" @close="editing = null" />
    <ToolPreviewDialog v-if="previewTool !== null" :tool="previewTool" @close="previewTool = null" />
    <WorkOffsetDialog v-if="offsetEditor !== null" :wcs="offsetEditor" @close="offsetEditor = null" />
    <OffsetHistoryDialog v-if="historyFilter !== null" :filter="historyFilter" @close="historyFilter = null" />

    <ModalDialog v-if="loading !== null" :title="t('tools.tab.loadTitle', { number: loading.number })" size="medium" expanded fresh-gesture @close="loading = null">
      <p class="confirm"><strong>{{ loading.comment || t('tools.tab.toolNumber', { number: loading.number }) }}</strong><br />{{ t('tools.tab.loadValues', { length: formatLength(loading.length, unit), diameter: formatLength(loading.diameter, unit), unit: unit === 'inch' ? 'in' : 'mm' }) }}</p>
      <p class="confirm">{{ t('tools.tab.loadEffect') }}</p>
      <p v-if="loadBlock" class="blocked" role="status">{{ loadBlock }}</p>
      <template #footer><button class="btn" type="button" @click="loading = null">{{ t('common.cancel') }}</button><button class="btn primary" type="button" :disabled="loadBlock !== null" @click="loadTool">{{ t('tools.tab.loadButton', { number: loading.number }) }}</button></template>
    </ModalDialog>

    <NumberDialog
      v-if="touching"
      :title="t('tools.tab.touchOffTitle', { number: loaded })"
      :effect="t('tools.tab.touchOffEffect', { number: loaded, system: wcs })"
      :label="t('tools.tab.touchOffLabel')"
      :unit="unit"
      :initial="touching.initial"
      :action="t('tools.tab.touchOffAction', { number: loaded })"
      @confirm="touchOff"
      @close="touching = null"
    />
  </div>
</template>

<style scoped>
.touched { display: inline-grid; place-items: center; width: 26px; height: 26px; border-radius: 50%; background: var(--go-soft); color: var(--go); vertical-align: middle; }
.touched :deep(svg) { width: 18px; height: 18px; stroke-width: 3; }
.tools {
  display: grid;
  grid-template-columns: minmax(0, 1fr) auto;
  gap: 10px;
  height: 100%;
  min-height: 0;
}

.heading {
  display: flex;
  align-items: center;
  gap: 10px;
  min-height: 44px;
}

.heading h2 {
  margin: 0;
  font-size: var(--type-body);
  font-weight: 600;
  color: var(--text);
}

.spacer {
  flex: 1;
}

.heading > .btn { min-height: 44px; white-space: nowrap; padding-inline: 10px; }
.table .heading { gap: 8px; }
.table .heading h2 { display: none; }
.offset-history { margin-left: auto; color: var(--warn); font-size: 12px; }
.heading > .history { margin-left: auto; }
.offset-history + .history { margin-left: 0; }
.heading > .tool-history { width: 44px; padding: 0; }
.table table { table-layout: auto; }
.table th { white-space: nowrap; }
.tool-column { width: 52px; }th.touched-column { width: 56px; text-align: center; }.touched-cell { text-align: center; }.dimension-column { width: 92px; }.load-column { width: 144px; }
.tool-row { cursor: pointer; }
.tool-row:not(.loaded):hover td, .tool-row.selected:not(.loaded) td { background: var(--accent-soft); }
.tool-row.selected td:first-child { box-shadow: inset 3px 0 var(--accent); }
.tool-number { width: 100%; min-height: 44px; padding: 0; background: transparent; border: 0; color: inherit; cursor: pointer; font: inherit; font-weight: 700; text-align: right; }
.tool-number:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
.offset-row.editable { cursor: pointer; }.offset-row.editable:hover td { background: var(--accent-soft); }.offset-row:focus-visible { outline: 2px solid var(--accent); outline-offset: -2px; }
.load-tool, .edit-tool { min-width: 64px; min-height: 44px; padding-inline: 8px; white-space: nowrap; }
.tool-row.selected .load-tool:not(:disabled, :hover, :active) { color: var(--accent); background: var(--panel-raised); }

.scroll {
  flex: 1;
  min-height: 0;
  overflow: auto;
}

table {
  width: 100%;
}

th {
  position: sticky;
  top: 0;
  padding: 6px;
  background: var(--bg-sunken);
  font-size: var(--type-sm);
  font-weight: 600;
  text-align: left;
  color: var(--text);
}

td {
  height: 47px;
  padding: 1px 6px;
  white-space: nowrap;
}

tr:nth-child(even):not(.loaded, .selected) td { background: color-mix(in srgb, var(--bg-sunken) 44%, var(--panel)); }

.num {
  text-align: right;
}

td.comment {
  white-space: normal;
  overflow-wrap: anywhere;
  font-size: var(--type-body);
  line-height: 1.3;
  padding-block: 6px;
}
.model-controls { display: flex; align-items: center; gap: 8px; padding-top: 10px; border-top: 1px solid var(--line); }
.model-controls .label { margin-right: auto; }
.tool-fields { display: grid; grid-template-columns: 1fr 1fr 1fr; gap: 12px; }
.tool-fields input { width: 100%; min-width: 0; }
.description-entry { grid-column: 1 / -1; }

tr.loaded td {
  background: var(--go-soft);
}

tr.loaded:not(.selected) td:first-child {
  box-shadow: inset 3px 0 var(--go);
}

.actions {
  text-align: right;
}

.actions .btn + .btn {
  margin-left: 4px;
}

.select {
  padding: 0 10px;
  font-size: var(--type-body);
  min-height: 44px;
}
.actions .chip + .btn { margin-left: 8px; }

.tools > .offsets { width: max-content; min-width: 480px; max-width: 600px; }
.offsets .select, .offsets .actions .btn { white-space: nowrap; }

.offsets td:first-child {
  font-weight: 600;
}

.offsets th, .offsets td { padding-left: 4px; padding-right: 4px; }
.offset-row.unconfirmed td.num { color: var(--text-dim); font-style: italic; }
.unconfirmed-note { cursor: help; }


.entry {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.confirm,
.blocked {
  margin: 0;
  line-height: 1.45;
}
</style>
