<script setup lang="ts">
import { vTooltip } from '../tooltip'
// Tools and offsets: the tool table, the tool in the spindle with its length touch-off, and the
// nine work coordinate systems.
import { computed, reactive, ref, shallowRef, watch } from 'vue'

import { startDialog } from '../dialogs'
import { vNumeric } from '../numeric-input'
import { findTool, linkBlock, mdiBlock, panel, send } from '../machine'
import type { Tool } from '../protocol'
import { resolveToolModel, toolDimensionError, toolFamilies } from '../tool-model'
import { toolModels } from '../tool-models'
import { formatLength, fromDisplay, parseLength, parseNumber, toDisplay, wcsName } from '../units'
import { workOffsetRows } from '../work-offsets'
import AppIcon from './AppIcon.vue'
import ModalDialog from './ModalDialog.vue'
import NumberDialog from './NumberDialog.vue'
import ToolModelDialog from './ToolModelDialog.vue'

const status = computed(() => panel.status)
const unit = computed(() => panel.unit)
const tools = computed(() => panel.toolTable?.tools ?? [])
const loaded = computed(() => status.value?.tool.in_spindle ?? 0)
const loadedTool = computed(() => findTool(loaded.value))
const wcs = computed(() => wcsName(status.value?.offsets.wcs ?? 1))
const applied = computed(() => status.value?.offsets.tool[2] ?? 0)
const compensation = computed(() => status.value?.active_codes.find((code) => /^G(?:43(?:\.[12])?|49)$/.test(code)) ?? '—')
const modelEditor = ref<{ tool: Tool | null } | null>(null)
const modelSettingsPending = computed(() => toolModels.storage === 'loading' || toolModels.saving)
function modelLabel(tool: Tool): string {
  const model = resolveToolModel(tool, toolModels.assignments.get(tool.number), toolModels.defaults)
  if (model === null) return 'Set model'
  if (toolDimensionError(model.family, model.dimensions) !== null) return 'Review model'
  return toolFamilies.find((f) => f.id === model.family)?.name ?? 'Set model'
}

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

const editing = ref<'add' | 'edit' | null>(null)
const editingTool = ref<Tool | null>(null)
const editUnit = ref(panel.unit)
const editHello = shallowRef(panel.hello)
const form = reactive({ number: '', length: '', diameter: '', comment: '' })
const initialDimensions = reactive({ length: '', diameter: '' })
function sameTool(tool: Tool): boolean {
  const current = findTool(tool.number)
  return current !== null && current.pocket === tool.pocket && current.length === tool.length &&
    current.diameter === tool.diameter && current.comment === tool.comment
}
const editorBlock = computed(() => editBlock.value ?? (panel.hello !== editHello.value ? 'The machine connection changed. Reopen this editor.' :
  editingTool.value !== null && !sameTool(editingTool.value) ? 'The tool table changed. Reopen this editor.' : null))

function edit(tool: Tool): void {
  if (editBlock.value !== null) return
  editingTool.value = { ...tool }
  editUnit.value = unit.value
  editHello.value = panel.hello
  form.number = String(tool.number)
  form.length = String(Number(toDisplay(tool.length, unit.value).toFixed(5)))
  form.diameter = String(Number(toDisplay(tool.diameter, unit.value).toFixed(5)))
  initialDimensions.length = form.length
  initialDimensions.diameter = form.diameter
  form.comment = tool.comment
  editing.value = 'edit'
}

function add(): void {
  if (editBlock.value !== null) return
  editingTool.value = null
  editUnit.value = unit.value
  editHello.value = panel.hello
  const free = tools.value.reduce((highest, tool) => Math.max(highest, tool.number), 0) + 1
  form.number = String(free)
  form.length = '0'
  form.diameter = '0'
  form.comment = ''
  editing.value = 'add'
}

if (startDialog === 'tool') {
  add()
}

const parsed = computed((): Tool | null => {
  const number = parseNumber(form.number)
  const length = editingTool.value !== null && form.length === initialDimensions.length
    ? editingTool.value.length : parseLength(form.length, editUnit.value)
  const diameter = editingTool.value !== null && form.diameter === initialDimensions.diameter
    ? editingTool.value.diameter : parseLength(form.diameter, editUnit.value)
  if (number === null || !Number.isInteger(number) || number < 1 || number > 2147483647 || length === null || diameter === null || diameter < 0) {
    return null
  }
  const existing = findTool(number)
  return { number, pocket: existing?.pocket ?? number, length, diameter, comment: form.comment.trim() }
})

const replaces = computed(
  () => editing.value === 'add' && parsed.value !== null && findTool(parsed.value.number) !== null,
)

function save(): void {
  if (parsed.value !== null && editorBlock.value === null) {
    send({ type: 'set_tool', tool: parsed.value })
    editing.value = null
  }
}

const removing = ref<Tool | null>(null)

function remove(): void {
  if (removing.value !== null && editorBlock.value === null && sameTool(removing.value) && removing.value.number !== loaded.value) {
    send({ type: 'remove_tool', number: removing.value.number })
    editing.value = null
  }
  removing.value = null
}

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
        <button v-tooltip="touchBlock ?? 'Set the loaded tool length from its present position'" class="btn touch-length" type="button" :disabled="touchBlock !== null" @click="openTouchOff">Touch off length</button>
        <button v-tooltip="editBlock ?? 'Add a tool'" class="btn" type="button" :disabled="editBlock !== null" @click="add">
          <AppIcon name="plus" /><span>Add tool</span>
        </button>
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
            <tr v-for="tool in tools" :key="tool.number" class="tool-row" :class="{ loaded: tool.number === loaded, editable: editBlock === null }" :tabindex="editBlock === null ? 0 : -1" :aria-label="`Edit tool ${tool.number}: ${tool.comment}`" @click="edit(tool)" @keydown.enter.self.prevent="edit(tool)" @keydown.space.self.prevent="edit(tool)">
              <td class="num mono">T{{ tool.number }}</td>
              <td class="num mono">{{ formatLength(tool.length, unit) }}</td>
              <td class="num mono">{{ formatLength(tool.diameter, unit) }}</td>
              <td class="comment">
                {{ tool.comment }}
              </td>
              <td class="actions">
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
            <tr v-for="system in systems" :key="system.wcs" :class="{ loaded: system.active }">
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
                  @click="send({ type: 'select_wcs', wcs: system.wcs })"
                >
                  Select
                </button>
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

    <ModalDialog v-if="editing !== null && modelEditor === null && removing === null" :title="editing === 'add' ? 'Add tool' : `Edit tool ${form.number}`" size="medium" expanded @close="editing = null">
      <div class="tool-fields">
        <label class="entry">
          <span class="label">Tool number</span>
          <input v-model="form.number" v-numeric="'integer'" class="field mono" type="text" inputmode="numeric" :disabled="editing === 'edit'" autocomplete="off" />
        </label>
        <label class="entry">
          <span class="label">Length, {{ editUnit }}</span>
          <input v-model="form.length" v-numeric :data-numeric-unit="editUnit" class="field mono" type="text" inputmode="decimal" autocomplete="off" />
        </label>
        <label class="entry">
          <span class="label">Diameter, {{ editUnit }}</span>
          <input v-model="form.diameter" v-numeric :data-numeric-unit="editUnit" class="field mono" type="text" inputmode="decimal" autocomplete="off" />
        </label>
        <label class="entry description-entry">
          <span class="label">Description</span>
          <input v-model="form.comment" class="field" type="text" autocomplete="off" />
        </label>
      </div>
      <p v-if="replaces" class="blocked">Tool {{ parsed?.number }} exists. Saving replaces it.</p>
      <div class="model-controls">
        <span class="label">Preview only</span>
        <button v-if="editingTool !== null" class="btn" type="button" :disabled="modelSettingsPending || editorBlock !== null" @click="modelEditor = { tool: editingTool }">{{ modelLabel(editingTool) }}</button>
        <button class="btn" type="button" :disabled="modelSettingsPending" @click="modelEditor = { tool: null }">Model defaults</button>
      </div>
      <p v-if="toolModels.storageError" class="blocked">{{ toolModels.storageError }}</p>
      <p v-else-if="toolModels.saved" class="hint" role="status">{{ toolModels.saved }}</p>
      <p v-else-if="toolModels.storageReason" class="hint">{{ toolModels.storageReason }}</p>
      <p v-if="editorBlock" class="blocked" role="status">{{ editorBlock }}</p>
      <template #footer>
        <button v-if="editingTool !== null" v-tooltip="editingTool.number === loaded ? 'The tool is in the spindle' : undefined" class="btn danger" type="button" :disabled="editorBlock !== null || editingTool.number === loaded" @click="removing = editingTool">Delete</button>
        <span class="spacer"></span>
        <button class="btn" type="button" @click="editing = null">Cancel</button>
        <button class="btn primary" type="button" :disabled="parsed === null || editorBlock !== null" @click="save">Save to tool table</button>
      </template>
    </ModalDialog>

    <ModalDialog v-if="removing !== null" :title="`Remove tool ${removing.number}?`" size="small" expanded fresh-gesture @close="removing = null">
      <p class="confirm">
        <span class="mono">T{{ removing.number }}</span> {{ removing.comment }} will be removed from the machine's tool
        table. Programs that call it will stop with an error.
      </p>
      <template #footer>
        <button class="btn" type="button" @click="removing = null">Cancel</button>
        <button class="btn danger" type="button" :disabled="editorBlock !== null || removing.number === loaded" @click="remove">Remove tool</button>
      </template>
    </ModalDialog>

    <ToolModelDialog v-if="modelEditor !== null" :tool="modelEditor.tool" @close="modelEditor = null" />

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
.tools {
  display: grid;
  grid-template-columns: minmax(0, 1fr) 412px;
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
.tool-column { width: 52px; }.dimension-column { width: 92px; }.load-column { width: 76px; }
.tool-row.editable { cursor: pointer; }
.tool-row.editable:hover td { background: var(--accent-soft); }
.tool-row:focus-visible { outline: 2px solid var(--accent); outline-offset: -2px; }
.load-tool { width: 64px; min-height: 44px; }

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
}

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
