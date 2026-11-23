<script setup lang="ts">
// Generate one operation or queue several in the order they will run.
import { computed, onBeforeUnmount, reactive, ref, shallowRef, watch } from 'vue'
import { useI18n } from 'vue-i18n'

import { translateMessage } from '../i18n/messages'

import { onResult, panel, send } from '../machine'
import { connectionBlock } from '../machine-block'
import { limitMarginMm, safeZBlock } from '../manual-moves'
import { vNumeric } from '../numeric-input'
import { operations, parse, uniqueName, unitOf, visibleFields, type Field, type Operation, type OperationId, type Values } from '../conversational/operations'
import { chainOperations, unitName, written, type ChainOperation, type Common, type OperationsKey, type Unit } from '../conversational/program'
import { byteLength, maxProgramBytes } from '../program-editor'
import type { CommandBody, Directory } from '../protocol'
import { fromDisplay, toDisplay } from '../units'
import { vTooltip } from '../tooltip'
import AppIcon from './AppIcon.vue'
import ModalDialog from './ModalDialog.vue'

const emit = defineEmits<{ close: [] }>()
const { t } = useI18n()

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
const operationNote = computed(() => operation.value.note?.(entered.value))

const tools = computed(() => [...panel.toolTable?.tools ?? []].sort((a, b) => a.number - b.number))
const spindle = computed(() => panel.status?.tool.in_spindle ?? 0)
// Until the operator chooses: the tool in the spindle, else one the tool table describes as
// fitting (the largest for facing, a drill for holes, an end mill otherwise).
const pickedTool = reactive<Partial<Record<OperationId, number>>>({})
function suggestedTool(id: OperationId): number {
  // The tool table diameter cannot identify these cutters.
  if (id === 'chamfer' || id === 'hole-finishing') return 0
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
  return { result: operation.value.generate(parsed, common), base: operation.value.name(parsed, unit), tool: common.tool.number, clearance: common.clearance }
})

interface JobOperation extends ChainOperation {
  id: number
  title: OperationsKey<'operation'>
  base: string
  clearance: number
}
const job = shallowRef<readonly JobOperation[]>([])
const reviewing = ref(false)
let nextOperation = 1
const toolChangeHeight = computed((): { z: number | null } | { error: string } => {
  const first = job.value[0]
  if (!first || job.value.every((op) => op.tool === first.tool)) return { z: null }
  const status = panel.status
  const max = status?.axes.find((axis) => axis.axis === 'z')?.max_limit
  const setting = panel.preferences.go_to_safe_z ?? (max ?? NaN) - limitMarginMm
  const z = written(toDisplay(setting, unit), unit)
  const machineZ = fromDisplay(z, unit)
  const reason = safeZBlock(status, setting, unit) ?? safeZBlock(status, machineZ, unit)
  if (reason !== null) return { error: reason }
  const offset = status ? status.offsets.g5x[2] + status.offsets.g92[2] : NaN
  // The program applies each tool's G43 length before moving to its work-coordinate clearance.
  if (job.value.some((op) => {
    const length = tools.value.find((row) => row.number === op.tool)?.length ?? NaN
    const clearance = offset + length + fromDisplay(written(op.clearance, unit), unit)
    return !Number.isFinite(clearance) || machineZ <= clearance + 1e-9
  })) return { error: t('operations.check.jobToolChangeHeight') }
  return { z }
})
const output = computed(() => {
  if (!job.value.length) return generated.value.result
  const height = toolChangeHeight.value
  return 'error' in height ? { errors: [height.error] } : chainOperations(job.value, unit, height.z)
})
const outputErrors = computed(() => {
  const result = output.value
  if ('errors' in result) return result.errors
  return byteLength(result.text) <= maxProgramBytes ? [] : [t('operations.check.jobTooLarge')]
})
const errors = computed(() => {
  const draft = generated.value.result
  return !reviewing.value && job.value.length && 'errors' in draft
    ? [...outputErrors.value, ...draft.errors]
    : outputErrors.value
})

function selectOperation(id: OperationId): void {
  chosen.value = id
  reviewing.value = false
}

function addToJob(): void {
  const { result, base, tool, clearance } = generated.value
  if (saving.value || 'errors' in result) return
  job.value = [...job.value, { id: nextOperation++, title: operation.value.title, base, tool, clearance, text: result.text }]
  feedback.value = ''
}

function removeFromJob(id: number): void {
  job.value = job.value.filter((op) => op.id !== id)
  if (!job.value.length) reviewing.value = false
  feedback.value = ''
}

// The program directory and the names in it, so a new program never takes an existing name.
const directory = ref<Directory | null>(null)
let listing: CommandBody | null = null
// Asked for once the link is live; asked again when it comes back after a failed listing.
function list(): void {
  if (panel.link !== 'live' || listing !== null || directory.value !== null) return
  listing = { type: 'list_directory', path: null }
  send(listing)
}
const fileName = computed(() => {
  const first = job.value[0]
  const base = !first ? generated.value.base : job.value.length === 1 ? first.base : `job-${first.base}` // i18n-ignore: generated filename
  return uniqueName(base, new Set(directory.value?.entries.map((entry) => entry.name)))
})

const saving = ref(false)
const feedback = ref('')
let saveRequest: CommandBody | null = null
const block = computed(() => connectionBlock(panel.link) ??
  (panel.status !== null && panel.status.interp !== 'idle' ? t('operations.dialog.stopProgram') : null))

function create(): void {
  const result = output.value
  const folder = directory.value?.path
  if ((job.value.length && !reviewing.value) || saving.value || 'errors' in result || outputErrors.value.length
    || folder === undefined || block.value !== null) return
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
    } else feedback.value = result.reason ? translateMessage(result.reason) : t('operations.dialog.directoryUnreadable')
  } else if (command === saveRequest) {
    saveRequest = null
    saving.value = false
    if (result.outcome !== 'accepted') {
      feedback.value = result.reason ? translateMessage(result.reason) : t('operations.dialog.notSaved')
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
const pocket = computed(() => fields.value.some((field) => field.key === 'entry'))
const fieldGroups = computed(() => {
  return [
    { label: t('operations.dialog.toolAndFeeds'), includesTool: true, fields: fields.value.filter((field) => cutFields.has(field.key)) },
    { label: pocket.value ? t('operations.dialog.pocketOptions') : t('operations.dialog.geometry'),
      includesTool: false, fields: fields.value.filter((field) => !cutFields.has(field.key)) },
  ]
})
</script>

<template>
  <ModalDialog :title="t('operations.dialog.title')" size="wide" contained expanded @close="emit('close')">
    <div class="operation-editor">
      <aside class="sidebar">
        <nav class="operations" :aria-label="t('operations.dialog.operations')">
          <button
            v-for="op in operations" :key="op.id" class="btn operation" type="button"
            :class="{ on: !reviewing && chosen === op.id }" :aria-pressed="!reviewing && chosen === op.id"
            :disabled="saving" @click="selectOperation(op.id)"
          >
            {{ t(op.title) }}
          </button>
        </nav>
        <button
          v-if="job.length" class="btn job-toggle" type="button" :class="{ on: reviewing }"
          :aria-pressed="reviewing" :disabled="saving" @click="reviewing = true"
        >
          {{ t('operations.dialog.job') }} <span class="mono">{{ job.length }}</span>
        </button>
      </aside>
      <section v-if="reviewing" class="form job" :aria-label="t('operations.dialog.job')">
        <ol>
          <li v-for="op in job" :key="op.id">
            <div class="job-row">
              <div class="job-description">
                <strong>{{ t(op.title) }}</strong>
                <span class="mono">T{{ op.tool }} · {{ op.base }}</span>
              </div>
              <button
                v-tooltip="t('operations.dialog.removeOperation', { name: t(op.title) })"
                class="btn quiet square" type="button" :disabled="saving"
                :aria-label="t('operations.dialog.removeOperation', { name: t(op.title) })"
                @click="removeFromJob(op.id)"
              >
                <AppIcon name="close" />
              </button>
            </div>
          </li>
        </ol>
        <p v-if="'z' in toolChangeHeight && toolChangeHeight.z !== null" class="job-height">
          {{ t('operations.dialog.toolChangeHeight', { z: toolChangeHeight.z, unit: unitName(unit) }) }}
        </p>
      </section>
      <div v-else class="form">
        <p v-if="operationNote" class="operation-note">{{ t(operationNote) }}</p>
        <fieldset v-for="group in fieldGroups" :key="group.label">
          <legend>{{ group.label }}</legend>
          <div
            class="fields"
            :class="{
              cutting: group.includesTool,
              pocket: pocket && !group.includesTool,
              chamfer: chosen === 'chamfer' && !group.includesTool,
            }"
          >
            <label v-if="group.includesTool" class="entry tool-entry">
              <span class="label">{{ t('operations.dialog.tool') }}</span>
              <select v-model.number="toolNumber" class="field" :aria-label="t('operations.dialog.tool')" :disabled="saving">
                <option v-if="tools.length === 0" :value="0">{{ t('operations.dialog.noTools') }}</option>
                <option v-else-if="chosen === 'chamfer'" :value="0" disabled>{{ t('operations.dialog.chooseChamferMill') }}</option>
                <option v-else-if="chosen === 'hole-finishing'" :value="0" disabled>{{ t('operations.dialog.chooseFinishTool') }}</option>
                <option v-for="row in tools" :key="row.number" :value="row.number">{{ toolText(row.number, row.diameter, row.comment) }}</option>
              </select>
            </label>
            <template v-for="field in group.fields" :key="`${chosen}-${field.key}-${field.label}`">
              <div v-if="field.type === 'choice'" class="entry">
                <span class="label">{{ t(field.label) }}</span>
                <select v-if="field.select || field.options.length > 3" v-model="entered[field.key]" class="field" :aria-label="t(field.label)" :disabled="saving">
                  <option v-for="[value, text] in field.options" :key="value" :value="value">{{ t(text) }}</option>
                </select>
                <div v-else class="segmented" role="group" :aria-label="t(field.label)">
                  <button
                    v-for="[value, text] in field.options" :key="value" class="btn" type="button" :disabled="saving"
                    :class="{ on: entered[field.key] === value }" :aria-pressed="entered[field.key] === value"
                    @click="entered[field.key] = value"
                  >
                    {{ t(text) }}
                  </button>
                </div>
              </div>
              <label v-else class="entry">
                <span class="label">{{ t(field.label) }}</span>
                <span class="number-field">
                  <input
                    v-model="entered[field.key]" v-numeric="field.kind === 'count' ? 'integer' : undefined"
                    :data-numeric-unit="numberUnit(field)" class="field mono" type="text"
                    :aria-label="numberUnit(field) ? t(field.label) + ', ' + numberUnit(field) : t(field.label)"
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
    <p v-else-if="feedback || block" class="errors" role="status">{{ feedback || (block && translateMessage(block)) }}</p>
    <template #footer>
      <div class="file">
        <template v-if="directory">
          <strong class="mono">{{ fileName }}</strong>
          <span v-tooltip="directory.path" class="where">{{ directory.path }}</span>
        </template>
      </div>
      <button class="btn" type="button" @click="emit('close')">{{ t('common.cancel') }}</button>
      <button v-if="!reviewing" class="btn" type="button" :disabled="saving || 'errors' in generated.result" @click="addToJob">
        {{ t('operations.dialog.addToJob') }}
      </button>
      <button v-if="job.length && !reviewing" class="btn primary" type="button" :disabled="saving" @click="reviewing = true">
        {{ t('operations.dialog.reviewJob') }}
      </button>
      <button v-else class="btn primary" type="button" :disabled="saving || outputErrors.length > 0 || directory === null || block !== null" @click="create">
        {{ saving ? t('common.saving') : job.length ? t('operations.dialog.createJob') : t('operations.dialog.createAndOpen') }}
      </button>
    </template>
  </ModalDialog>
</template>

<style scoped>
.operation-editor { display: grid; grid-template-columns: 190px minmax(0, 1fr); gap: 20px; flex: 1; min-height: 0; }
.sidebar { min-height: 0; overflow-y: auto; }
.operations { display: flex; flex-direction: column; gap: 6px; }
.operation { justify-content: flex-start; min-height: 48px; }
.job-toggle { width: 100%; min-height: 48px; margin-top: 12px; justify-content: space-between; }
.job-description span { color: var(--text-dim); font-size: var(--type-sm); overflow-wrap: anywhere; }
.job ol { display: grid; margin: 0; padding-left: 26px; }
.job-height { margin: 0; color: var(--text-dim); font-size: var(--type-sm); }
.job li { padding-left: 8px; }
.job-row { display: flex; align-items: center; justify-content: space-between; gap: 12px; min-height: 56px; border-bottom: 1px solid var(--line); }
.job-description { display: grid; min-width: 0; gap: 2px; }
.job-row .btn { flex: none; }
.form { display: flex; flex-direction: column; gap: 10px; min-width: 0; min-height: 0; overflow-y: auto; padding-right: 4px; }
.form fieldset { min-width: 0; margin: 0; padding: 0; border: 0; }
.form legend { width: 100%; margin-bottom: 7px; padding: 5px 0 0; border-top: 1px solid var(--line); color: var(--text); font-size: 13px; font-weight: 600; }
.fields { display: grid; grid-template-columns: repeat(5, minmax(0, 1fr)); gap: 8px 12px; align-content: start; }
.fields.cutting, .fields.chamfer { grid-template-columns: repeat(6, minmax(0, 1fr)); }
.fields.pocket { grid-template-columns: repeat(7, minmax(0, 1fr)); }
.fields.chamfer .entry { justify-content: flex-end; }
.fields.pocket .number-field:has(.field-unit) input { padding-right: 44px; }
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
@media (max-width: 1000px) { .operation-editor { grid-template-columns: 150px minmax(0, 1fr); gap: 12px; }.fields, .fields.cutting, .fields.pocket, .fields.chamfer { grid-template-columns: repeat(3, minmax(0, 1fr)); } }
</style>
