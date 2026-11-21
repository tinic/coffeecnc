<script setup lang="ts">
// A simple job without CAM: the operator picks an operation and its numbers, the panel writes
// the program into the program directory and opens it like any other program.
import { computed, onBeforeUnmount, reactive, ref, watch } from 'vue'

import { onResult, panel, send } from '../machine'
import { connectionBlock } from '../machine-block'
import { vNumeric } from '../numeric-input'
import { operations, parse, uniqueName, unitOf, visibleFields, type Field, type Operation, type OperationId, type Values } from '../conversational/operations'
import type { Common, Unit } from '../conversational/program'
import type { CommandBody, Directory } from '../protocol'
import { toDisplay } from '../units'
import { vTooltip } from '../tooltip'
import ModalDialog from './ModalDialog.vue'

const emit = defineEmits<{ close: [] }>()

// The program is written in the unit the panel shows, with G20 or G21 at its start. The entered
// numbers are in that unit, so it stays fixed while the dialog is open.
const unit: Unit = panel.unit
const chosen = ref<OperationId>('facing')
const operation = computed((): Operation => {
  const found = operations.find((candidate) => candidate.id === chosen.value)
  if (found === undefined) throw new Error(`Unknown operation ${chosen.value}`)
  return found
})
const maxRpm = panel.hello?.max_spindle_rpm
const presetRpm = panel.preferences.default_spindle_rpm
const values = reactive(Object.fromEntries(operations.map((op) => {
  const entered = op.defaults(unit)
  const rpm = presetRpm > 0 ? presetRpm : Number(entered['rpm'])
  entered['rpm'] = String(maxRpm !== undefined && maxRpm > 0 ? Math.min(rpm, maxRpm) : rpm)
  return [op.id, entered]
})) as Record<OperationId, Values>)
const entered = computed(() => values[chosen.value])
const fields = computed(() => visibleFields(operation.value, entered.value))

const tools = computed(() => [...panel.toolTable?.tools ?? []].sort((a, b) => a.number - b.number))
const spindle = computed(() => panel.status?.tool.in_spindle ?? 0)
// Until the operator chooses: the tool in the spindle, else one the tool table describes as
// fitting (the largest for facing, a drill for holes, an end mill otherwise).
const pickedTool = reactive<Partial<Record<OperationId, number>>>({})
function suggestedTool(id: OperationId): number {
  const cutting = tools.value.filter((tool) => tool.diameter > 0)
  const named = (wanted: RegExp, unwanted: RegExp): number | undefined =>
    cutting.find((tool) => wanted.test(tool.comment) && !unwanted.test(tool.comment))?.number
  const loaded = cutting.find((tool) => tool.number === spindle.value)?.number
  const largest = [...cutting].sort((a, b) => b.diameter - a.diameter)[0]?.number
  const fitting = id === 'facing' ? largest : id === 'drilling' ? named(/drill/i, /spot|cent(er|re)/i) : named(/mill/i, /face/i)
  return loaded ?? fitting ?? cutting[0]?.number ?? tools.value[0]?.number ?? 0
}
const toolNumber = computed({
  get: (): number => pickedTool[chosen.value] ?? suggestedTool(chosen.value),
  set: (number: number): void => { pickedTool[chosen.value] = number },
})
const tool = computed(() => tools.value.find((candidate) => candidate.number === toolNumber.value))
function toolText(number: number, diameter: number, comment: string): string {
  return `T${String(number)} · ⌀ ${String(Number(toDisplay(diameter, unit).toFixed(unit === 'inch' ? 4 : 3)))} ${unit === 'inch' ? 'in' : 'mm'}${comment ? ` · ${comment}` : ''}`
}

const generated = computed(() => {
  const parsed = parse(operation.value, entered.value, unit)
  const selected = tool.value
  const common: Common = {
    unit, rpm: parsed.num('rpm'), feed: parsed.num('feed'), clearance: parsed.num('clearance'),
    tool: { number: selected?.number ?? 0, diameter: selected ? toDisplay(selected.diameter, unit) : NaN, comment: selected?.comment ?? '' },
  }
  return { result: operation.value.generate(parsed, common), base: operation.value.name(parsed, unit) }
})
const errors = computed(() => 'errors' in generated.value.result ? generated.value.result.errors : [])

// The program directory and the names in it, so a new program never takes an existing name.
const directory = ref<Directory | null>(null)
let listing: CommandBody | null = null
// Asked for once the link is live; asked again when it comes back after a failed listing.
function list(): void {
  if (panel.link !== 'live' || listing !== null || directory.value !== null) return
  listing = { type: 'list_directory', path: null }
  send(listing)
}
const fileName = computed(() => uniqueName(generated.value.base, new Set(directory.value?.entries.map((entry) => entry.name))))

const saving = ref(false)
const feedback = ref('')
let saveRequest: CommandBody | null = null
const block = computed(() => connectionBlock(panel.link) ??
  (panel.status !== null && panel.status.interp !== 'idle' ? 'Stop the program before opening a new one.' : null))

function create(): void {
  const result = generated.value.result
  const folder = directory.value?.path
  if (saving.value || 'errors' in result || folder === undefined || block.value !== null) return
  saving.value = true
  feedback.value = ''
  const path = `${folder === '/' ? '' : folder}/${fileName.value}`
  saveRequest = { type: 'save_program', path, text: result.text }
  send(saveRequest)
}

const removeListener = onResult((command, result) => {
  if (command === listing) {
    listing = null
    if (result.outcome === 'accepted' && panel.directory !== null) {
      directory.value = panel.directory
      feedback.value = ''
    } else feedback.value = result.reason || 'The program directory cannot be read.'
  } else if (command === saveRequest) {
    saveRequest = null
    saving.value = false
    if (result.outcome !== 'accepted') {
      feedback.value = result.reason || 'The program was not saved.'
      return
    }
    // Saved: open it as the current program; Program check and the preview follow.
    send({ type: 'open_program', path: command.type === 'save_program' ? command.path : '' })
    emit('close')
  }
})
onBeforeUnmount(removeListener)
watch(() => panel.link, list, { immediate: true })

function numberUnit(field: Field): string {
  return field.type === 'number' ? unitOf(field.kind, unit) : ''
}
const cutFields = new Set(['feed', 'plungeFeed', 'rpm', 'clearance'])
const fieldGroups = computed(() => [
  { label: 'Tool and feeds', includesTool: true, fields: fields.value.filter((field) => cutFields.has(field.key)) },
  { label: 'Geometry', includesTool: false, fields: fields.value.filter((field) => !cutFields.has(field.key)) },
])
</script>

<template>
  <ModalDialog title="New operation" size="wide" contained expanded @close="emit('close')">
    <div class="operation-editor">
      <nav class="operations" aria-label="Operation">
        <button
          v-for="op in operations" :key="op.id" class="btn operation" type="button"
          :class="{ on: chosen === op.id }" :aria-pressed="chosen === op.id" @click="chosen = op.id"
        >
          {{ op.title }}
        </button>
      </nav>
      <div class="form">
        <p v-if="operation.note" class="operation-note">{{ operation.note }}</p>
        <fieldset v-for="group in fieldGroups" :key="group.label">
          <legend>{{ group.label }}</legend>
          <div class="fields" :class="{ cutting: group.includesTool }">
            <label v-if="group.includesTool" class="entry tool-entry">
              <span class="label">Tool</span>
              <select v-model.number="toolNumber" class="field" aria-label="Tool" :disabled="saving">
                <option v-if="tools.length === 0" :value="0">No tools in the tool table</option>
                <option v-for="row in tools" :key="row.number" :value="row.number">{{ toolText(row.number, row.diameter, row.comment) }}</option>
              </select>
            </label>
            <template v-for="field in group.fields" :key="`${chosen}-${field.key}-${field.label}`">
              <div v-if="field.type === 'choice'" class="entry">
                <span class="label">{{ field.label }}</span>
                <select v-if="field.options.length > 3" v-model="entered[field.key]" class="field" :aria-label="field.label" :disabled="saving">
                  <option v-for="[value, text] in field.options" :key="value" :value="value">{{ text }}</option>
                </select>
                <div v-else class="segmented" role="group" :aria-label="field.label">
                  <button
                    v-for="[value, text] in field.options" :key="value" class="btn" type="button" :disabled="saving"
                    :class="{ on: entered[field.key] === value }" :aria-pressed="entered[field.key] === value"
                    @click="entered[field.key] = value"
                  >
                    {{ text }}
                  </button>
                </div>
              </div>
              <label v-else class="entry">
                <span class="label">{{ field.label }}</span>
                <span class="number-field">
                  <input
                    v-model="entered[field.key]" v-numeric="field.kind === 'count' ? 'integer' : undefined"
                    :data-numeric-unit="numberUnit(field)" class="field mono" type="text"
                    :aria-label="numberUnit(field) ? field.label + ', ' + numberUnit(field) : field.label"
                    :inputmode="field.kind === 'count' ? 'numeric' : 'decimal'" autocomplete="off" spellcheck="false" :disabled="saving"
                  />
                  <span v-if="numberUnit(field)" class="field-unit" aria-hidden="true">{{ numberUnit(field) }}</span>
                </span>
              </label>
            </template>
          </div>
        </fieldset>
      </div>
    </div>
    <ul v-if="errors.length" class="errors" role="alert">
      <li v-for="error in errors" :key="error">{{ error }}</li>
    </ul>
    <p v-else-if="feedback || block" class="errors" role="status">{{ feedback || block }}</p>
    <template #footer>
      <div class="file">
        <template v-if="directory">
          <strong class="mono">{{ fileName }}</strong>
          <span v-tooltip="directory.path" class="where">{{ directory.path }}</span>
        </template>
      </div>
      <button class="btn" type="button" @click="emit('close')">Cancel</button>
      <button class="btn primary" type="button" :disabled="saving || errors.length > 0 || directory === null || block !== null" @click="create">
        {{ saving ? 'Saving…' : 'Create and open' }}
      </button>
    </template>
  </ModalDialog>
</template>

<style scoped>
.operation-editor { display: grid; grid-template-columns: 190px minmax(0, 1fr); gap: 20px; flex: 1; min-height: 0; }
.operations { display: flex; flex-direction: column; gap: 6px; min-height: 0; overflow-y: auto; }
.operation { justify-content: flex-start; min-height: 48px; }
.form { display: flex; flex-direction: column; gap: 10px; min-width: 0; min-height: 0; overflow-y: auto; padding-right: 4px; }
.form fieldset { min-width: 0; margin: 0; padding: 0; border: 0; }
.form legend { width: 100%; margin-bottom: 7px; padding: 5px 0 0; border-top: 1px solid var(--line); color: var(--text); font-size: 13px; font-weight: 600; }
.fields { display: grid; grid-template-columns: repeat(5, minmax(0, 1fr)); gap: 8px 12px; align-content: start; }
.fields.cutting { grid-template-columns: repeat(6, minmax(0, 1fr)); }
.tool-entry { grid-column: span 2; }
.number-field { position: relative; min-width: 0; }
.number-field:has(.field-unit) input { padding-right: 62px; }
.field-unit { position: absolute; right: 9px; top: 50%; transform: translateY(-50%); color: var(--text-dim); font-size: 11px; pointer-events: none; }
.entry { display: flex; flex-direction: column; gap: 5px; min-width: 0; }
.operation-note { margin: 0; font-size: 13px; color: var(--warn); }
.entry .field { width: 100%; min-width: 0; min-height: 44px; }
.segmented { display: flex; }.segmented .btn { flex: 1; min-width: 0; padding: 0 8px; }
.errors { margin: 0; padding: 0 0 0 18px; color: var(--warn); font-size: 12px; line-height: 1.5; }
p.errors { padding: 0; }
.file { flex: 1; min-width: 0; overflow: hidden; align-self: center; font-size: 12px; }
.file strong, .where { display: block; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.file strong { font-size: 13px; font-weight: 500; }
.where { margin-top: 2px; color: var(--text-dim); }
@media (max-width: 1000px) { .operation-editor { grid-template-columns: 150px minmax(0, 1fr); gap: 12px; }.fields, .fields.cutting { grid-template-columns: repeat(3, minmax(0, 1fr)); } }
</style>
