<script setup lang="ts">
import { vTooltip } from '../tooltip'
// Tools and offsets: the tool table, the tool in the spindle with its length touch-off, and the
// nine work coordinate systems.
import { computed, ref, watch } from 'vue'

import { startDialog } from '../dialogs'
import { findTool, linkBlock, mdiBlock, panel, send } from '../machine'
import type { Tool } from '../protocol'
import { formatLength, fromDisplay, toDisplay, wcsName } from '../units'
import { workOffsetRows } from '../work-offsets'
import AppIcon from './AppIcon.vue'
import FixedSetterControls from './FixedSetterControls.vue'
import ModalDialog from './ModalDialog.vue'
import NumberDialog from './NumberDialog.vue'
import ToolEditor from './ToolEditor.vue'
import WorkOffsetDialog from './WorkOffsetDialog.vue'
import { setterStatus, touchedOff } from '../setter-state'

const status = computed(() => panel.status)
const unit = computed(() => panel.unit)
const tools = computed(() => panel.toolTable?.tools
  .filter((tool) => tool.number > 0)
  .sort((a, b) => a.number - b.number) ?? [])
const selectedTool = ref<number | null>(null)
watch(tools, (rows) => {
  if (!rows.some((tool) => tool.number === selectedTool.value)) selectedTool.value = null
})
const loaded = computed(() => status.value?.tool.in_spindle ?? 0)
const loadedTool = computed(() => findTool(loaded.value))
const wcs = computed(() => wcsName(status.value?.offsets.wcs ?? 1))
const applied = computed(() => status.value?.offsets.tool[2] ?? 0)
const compensation = computed(() => status.value?.active_codes.find((code) => /^G(?:43(?:\.[12])?|49)$/.test(code)) ?? '—')
/** Why the tool table cannot be edited, or null. */
const editBlock = computed((): string | null => {
  if (linkBlock.value !== null || status.value === null) {
    return linkBlock.value ?? 'No status from the machine yet'
  }
  return status.value.interp === 'idle' ? null : 'A program or MDI command is running'
})

const touchBlock = computed((): string | null => {
  if (mdiBlock.value !== null) {
    return mdiBlock.value
  }
  return loaded.value === 0 ? 'No tool in the spindle' : null
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
  ? 'The tool table changed. Select the tool again.' : null))
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
const rotated = computed(() => systems.value.some((system) => system.rotation_xy !== null && system.rotation_xy !== 0))
const offsetEditor = ref<number | null>(null)
function editOffset(wcs: number): void {
  if (mdiBlock.value === null && panel.link === 'live' && status.value?.link === 'live') offsetEditor.value = wcs
}
</script>

<template>
  <div class="tools">
    <section class="card table">
      <div class="heading">
        <h2>Tool table</h2>
        <span class="hint">{{ unit }}</span>
        <span class="spindle-line" :class="{ stale: panel.link !== 'live' }">
          <template v-if="loaded !== 0"><strong class="mono">T{{ loaded }}</strong> in spindle · Length <span class="mono">{{ formatLength(applied, unit) }}</span> {{ unit === 'inch' ? 'in' : 'mm' }} · {{ compensation }}</template>
          <template v-else>No tool in spindle</template>
        </span>
        <button v-if="!panel.hello?.fixed_setter" v-tooltip="touchBlock ?? 'Set the loaded tool length from its present position'" class="btn touch-length" type="button" :disabled="touchBlock !== null" @click="openTouchOff">Touch off length</button>
        <FixedSetterControls v-if="panel.hello?.fixed_setter" :limits="panel.hello.fixed_setter" />
        <button v-tooltip="editBlock ?? 'Add a tool'" class="btn" type="button" :disabled="editBlock !== null" @click="add">
          <AppIcon name="plus" /><span>Add tool</span>
        </button>
      </div>
      <div v-if="panel.hello?.fixed_setter" class="setter-band" :class="setterStatus.level" role="status" aria-live="polite">
        <span class="setter-label">{{ setterStatus.level === 'blocked' ? '!' : setterStatus.level === 'done' ? '✓' : 'i' }}</span>
        <p>{{ setterStatus.text }}</p>
      </div>

      <p v-if="lengthMismatch" class="blocked">Applied length differs from the table. Load the tool again to apply its current length.</p>

      <div class="scroll">
        <table>
          <thead>
            <tr>
              <th class="num tool-column">Tool</th>
              <th v-tooltip="'LinuxCNC tool-length offset, not physical tool length'" class="num dimension-column">Length</th>
              <th class="num dimension-column">Diameter</th>
              <th>Description</th>
              <th class="load-column"></th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="tool in tools" :key="tool.number" class="tool-row" :class="{ loaded: tool.number === loaded, selected: tool.number === selectedTool }" @click="selectedTool = tool.number">
              <td class="num mono">
                <button class="tool-number" type="button" :aria-label="`Select tool ${tool.number}`" :aria-pressed="tool.number === selectedTool" @click.stop="selectedTool = tool.number">T{{ tool.number }}</button>
                <span v-if="touchedOff(tool.number)" class="touched" title="Touched off against the current reference" aria-label="Touched off">✓</span>
              </td>
              <td class="num mono">{{ formatLength(tool.length, unit) }}</td>
              <td class="num mono">{{ formatLength(tool.diameter, unit) }}</td>
              <td class="comment">
                {{ tool.comment }}
              </td>
              <td class="actions">
                <button v-tooltip="editBlock ?? `Edit T${tool.number}`" class="btn edit-tool" type="button" :disabled="editBlock !== null" :aria-label="`Edit tool ${tool.number}`" @click.stop="edit(tool)">Edit</button>
                <button v-tooltip="mdiBlock ?? `Load T${tool.number} and apply its length`" class="btn load-tool" type="button" :disabled="mdiBlock !== null" :aria-label="`Load tool ${tool.number}`" @click.stop="openLoad(tool)">Load</button>
              </td>
            </tr>
          </tbody>
        </table>
        <p v-if="panel.toolTable === null" class="hint">No tool table received.</p>
      </div>
    </section>

    <section class="card offsets">
      <div class="heading">
        <h2>Work offsets</h2>
        <span class="hint">{{ unit }} · origins, in machine coordinates</span>
      </div>
      <div class="scroll">
        <table>
          <thead>
            <tr>
              <th>System</th>
              <th class="num">X</th>
              <th class="num">Y</th>
              <th class="num">Z</th>
              <th v-if="rotated" class="num">Rot</th>
              <th></th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="system in systems" :key="system.wcs" class="offset-row" :class="{ loaded: system.active, editable: mdiBlock === null && panel.link === 'live' && status?.link === 'live' }" :tabindex="mdiBlock === null && panel.link === 'live' && status?.link === 'live' ? 0 : -1" :aria-label="`Edit ${wcsName(system.wcs)} work origins`" @click="editOffset(system.wcs)" @keydown.enter.self.prevent="editOffset(system.wcs)" @keydown.space.self.prevent="editOffset(system.wcs)">
              <td class="mono">{{ wcsName(system.wcs) }}<small v-if="system.offset === null" class="offset-note">Unavailable</small><small v-else-if="system.stale" class="offset-note stale">Stale</small></td>
              <td v-for="(value, axis) in system.offset ?? [null, null, null]" :key="axis" class="num mono">{{ value === null ? '—' : formatLength(value, unit) }}</td>
              <td v-if="rotated" class="num mono">{{ system.rotation_xy === null ? '—' : system.rotation_xy === 0 ? '' : `${system.rotation_xy}°` }}</td>
              <td class="actions">
                <span v-if="system.active" class="chip go">active</span>
                <button
                  v-else
                  v-tooltip="mdiBlock ?? `Make ${wcsName(system.wcs)} the active work system. The machine does not move.`"
                  class="btn select"
                  type="button"
                  :disabled="mdiBlock !== null || status?.link !== 'live'"
                  @click.stop="send({ type: 'select_wcs', wcs: system.wcs })"
                >
                  Select
                </button>
                <button class="btn select" type="button" :aria-label="`Edit ${wcsName(system.wcs)} origins`" :disabled="mdiBlock !== null || panel.link !== 'live' || status?.link !== 'live'" @click.stop="editOffset(system.wcs)">Edit</button>
              </td>
            </tr>
          </tbody>
        </table>
        <p v-if="panel.workOffsets === null" class="hint">No work offsets received.</p>
      </div>
      <p v-if="status?.offsets.g92.some((v) => v !== 0)" class="blocked g92">
        A G92/G52 shift of
        <span class="mono">{{ status.offsets.g92.map((v) => formatLength(v, unit)).join('  ') }}</span>
        is added to every system.
      </p>
    </section>

    <ToolEditor v-if="editing !== null" :tool="editing.tool" @close="editing = null" />
    <WorkOffsetDialog v-if="offsetEditor !== null" :wcs="offsetEditor" @close="offsetEditor = null" />

    <ModalDialog v-if="loading !== null" :title="`Load T${loading.number}?`" size="medium" expanded fresh-gesture @close="loading = null">
      <p class="confirm"><strong>{{ loading.comment || `Tool ${loading.number}` }}</strong><br />Length {{ formatLength(loading.length, unit) }} {{ unit === 'inch' ? 'in' : 'mm' }} · Diameter {{ formatLength(loading.diameter, unit) }} {{ unit === 'inch' ? 'in' : 'mm' }}</p>
      <p class="confirm">Starts the machine’s tool-change sequence and applies this tool’s length. Follow the tool-change prompt if shown. The configured sequence may move the axes; clear the tool-change area first.</p>
      <p v-if="loadBlock" class="blocked" role="status">{{ loadBlock }}</p>
      <template #footer><button class="btn" type="button" @click="loading = null">Cancel</button><button class="btn primary" type="button" :disabled="loadBlock !== null" @click="loadTool">Load T{{ loading.number }}</button></template>
    </ModalDialog>

    <NumberDialog
      v-if="touching"
      :title="`Touch off the length of T${loaded}`"
      :effect="`Sets the length of T${loaded} in the tool table so that Z reads the value below in ${wcs} at the present position, and applies it (G43). The machine does not move.`"
      label="Z is now at"
      :unit="unit"
      :initial="touching.initial"
      :action="`Set T${loaded} length`"
      @confirm="touchOff"
      @close="touching = null"
    />
  </div>
</template>

<style scoped>
/* The setter's status line: the same band as the system messages, fixed height. */
.setter-band { display: flex; align-items: center; gap: 12px; height: 40px; margin: 6px 0 4px; padding: 0 10px; border: 1px solid var(--line); border-radius: 9px; background: var(--panel); color: var(--text-dim); }
.setter-band.blocked { color: var(--stop); background: var(--stop-soft); border-color: var(--stop); border-left-width: 5px; }
.setter-band.done { color: var(--ok, #15803d); }
.setter-band.busy { color: var(--accent); }
.setter-label { display: grid; place-items: center; flex: none; width: 24px; height: 24px; border: 1px solid currentColor; border-radius: 50%; font-weight: 700; }
.setter-band p { flex: 1; min-width: 0; margin: 0; font-size: 13px; line-height: 20px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
.touched { margin-left: 6px; color: var(--ok, #15803d); font-size: 20px; font-weight: 700; line-height: 1; vertical-align: middle; }
.tools {
  display: grid;
  grid-template-columns: minmax(0, 1fr) 480px;
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
  font-size: 11px;
  font-weight: 600;
  letter-spacing: 0.07em;
  text-transform: uppercase;
  color: var(--text-dim);
}

.spacer {
  flex: 1;
}

.spindle-line { flex: 1; min-width: 0; font-size: 12px; line-height: 1.5; color: var(--text-dim); }
.spindle-line strong { color: var(--text); font-size: 14px; }
.spindle-line.stale strong { color: var(--text-faint); }
.heading > .btn { min-height: 44px; white-space: nowrap; padding-inline: 10px; }
.table .heading { gap: 8px; }
.table .heading h2 { display: none; }
.table table { table-layout: fixed; }
.tool-column { width: 52px; }.dimension-column { width: 92px; }.load-column { width: 144px; }
.tool-row { cursor: pointer; }
.tool-row:not(.loaded):hover td, .tool-row.selected:not(.loaded) td { background: var(--accent-soft); }
.tool-row.selected td:first-child { box-shadow: inset 3px 0 var(--accent); }
.tool-number { width: 100%; min-height: 44px; padding: 0; background: transparent; border: 0; color: inherit; cursor: pointer; font: inherit; text-align: right; }
.tool-number:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
.offset-row.editable { cursor: pointer; }.offset-row.editable:hover td { background: var(--accent-soft); }.offset-row:focus-visible { outline: 2px solid var(--accent); outline-offset: -2px; }
.load-tool, .edit-tool { width: 64px; min-height: 44px; }

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
  border-bottom: 1px solid var(--line-strong);
  background: var(--panel);
  font-size: 11px;
  font-weight: 600;
  letter-spacing: 0.07em;
  text-align: left;
  text-transform: uppercase;
  color: var(--text-dim);
}

td {
  height: 47px;
  padding: 1px 6px;
  border-bottom: 1px solid var(--line);
  white-space: nowrap;
}

.num {
  text-align: right;
}

td.comment {
  white-space: normal;
  overflow-wrap: anywhere;
  font-size: 13px;
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

.actions {
  text-align: right;
}

.actions .btn + .btn {
  margin-left: 4px;
}

.select {
  padding: 0 10px;
  font-size: 13px;
  min-height: 44px;
}
.actions .chip + .btn { margin-left: 8px; }

.offsets td:first-child {
  font-weight: 600;
}

.offsets th, .offsets td { padding-left: 4px; padding-right: 4px; }

.offset-note { display: block; font: 10px var(--sans); font-weight: 400; color: var(--text-dim); }
.offset-note.stale { color: var(--warn); }

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
