<script setup lang="ts">
// Saved positions: the present machine position under a name, kept per machine in the operator
// settings. Go to lifts Z to a safe height, moves X and Y, and lowers Z only when asked.
import { computed, onBeforeUnmount, ref, shallowRef, watch } from 'vue'

import { useCardFeedback } from '../card-feedback'
import { mdiBlock, onResult, operatorSettings, panel, send } from '../machine'
import { powerBlock } from '../machine-block'
import { chosenPositionName, goToPlan, machinePosition, nextPositionName, saveBlock, type GoToPlan } from '../manual-moves'
import { createMoveSequence } from '../move-sequence'
import { maxPositionNameBytes, type SavedPosition } from '../operator-settings'
import type { SetterStatusLine } from '../setter-state'
import { vTooltip } from '../tooltip'
import { formatLength } from '../units'
import AppIcon from './AppIcon.vue'
import ModalDialog from './ModalDialog.vue'
import NameEntry from './NameEntry.vue'

const emit = defineEmits<{ line: [SetterStatusLine] }>()

const unit = computed(() => panel.unit)
const unitName = computed(() => unit.value === 'inch' ? 'in' : 'mm')
const positions = computed(() => panel.preferences.saved_positions)
const length = (mm: number): string => formatLength(mm, unit.value)

const sequence = createMoveSequence({ send, onResult, status: () => panel.status, block: () => powerBlock(panel) })
// Leaving the card stops a Go to: nothing shows its moves any more, so none may go on. Stop in
// the run bar stays available throughout.
onBeforeUnmount(() => {
  if (sequence.state.running) send({ type: 'abort' })
  sequence.dispose()
})

const saving = ref(false)
const goToBlock = computed(() => sequence.state.running ? 'Go to is moving' : mdiBlock.value)
const addBlock = computed(() => saving.value ? 'Saving…' : mdiBlock.value ?? saveBlock(panel.status, positions.value))

const target = shallowRef<SavedPosition | null>(null)
const progress = computed(() => sequence.state.running && target.value
  ? `Go to ${target.value.name}: move ${String(sequence.state.step + 1)} of ${String(sequence.state.total)}` : '')
const { line, allow } = useCardFeedback(
  () => '',
  () => null,
  [mdiBlock, positions],
)
const arrivedAt = ref('')
watch(() => sequence.state.running, (running) => {
  arrivedAt.value = !running && sequence.state.error === '' && target.value ? `At ${target.value.name}` : ''
})
const shownLine = computed((): SetterStatusLine => {
  if (progress.value) return { text: progress.value, level: 'busy' }
  if (line.value.level === 'blocked') return line.value
  if (sequence.state.error) return { text: sequence.state.error, level: 'blocked' }
  return arrivedAt.value && line.value.text === '' ? { text: arrivedAt.value, level: 'done' } : line.value
})
watch(shownLine, (value) => { emit('line', value) }, { immediate: true })

/** A new action replaces the outcome of the last one. */
function act(reason: string | null): boolean {
  sequence.state.error = ''
  arrivedAt.value = ''
  return allow(reason)
}

async function store(next: SavedPosition[]): Promise<void> {
  saving.value = true
  const result = await operatorSettings.save({ ...panel.preferences, saved_positions: next }, operatorSettings.context())
  saving.value = false
  if (!result.saved) allow(result.reason || 'The positions could not be saved')
}

// ---- save and rename ---------------------------------------------------------------------------

/** A new position (`renaming` null) or a saved one being renamed; `fallback` is the empty name. */
const draft = shallowRef<{ fallback: string; position: SavedPosition['position']; renaming: SavedPosition | null } | null>(null)
const draftName = ref('')
const chosenName = computed(() => draft.value === null ? null
  : chosenPositionName(draftName.value, draft.value.fallback, positions.value, draft.value.renaming))
const nameBlock = computed(() => saving.value ? 'Saving…' : chosenName.value?.ok === false ? chosenName.value.reason : null)

function openSave(): void {
  if (!act(addBlock.value) || panel.status === null) return
  const name = nextPositionName(positions.value)
  draft.value = { fallback: name, position: machinePosition(panel.status), renaming: null }
  draftName.value = name
}

function openRename(entry: SavedPosition): void {
  if (!act(saving.value ? 'Saving…' : null)) return
  draft.value = { fallback: entry.name, position: entry.position, renaming: entry }
  draftName.value = entry.name
}

function confirmName(): void {
  const current = draft.value
  const chosen = chosenName.value
  if (current === null || chosen === null || !chosen.ok || saving.value) return
  draft.value = null
  const { renaming } = current
  if (renaming === null) {
    void store([...positions.value, { name: chosen.plan, position: current.position }])
  } else if (chosen.plan !== renaming.name) {
    void store(positions.value.map((entry) => entry === renaming ? { ...entry, name: chosen.plan } : entry))
  }
}

// ---- delete ------------------------------------------------------------------------------------

const removal = shallowRef<SavedPosition | null>(null)

function confirmDelete(): void {
  const entry = removal.value
  removal.value = null
  if (entry === null || !act(saving.value ? 'Saving…' : null)) return
  void store(positions.value.filter((candidate) => candidate !== entry))
}

// ---- go to -------------------------------------------------------------------------------------

const includeZ = ref(true)
const goTo = computed((): GoToPlan | string | null => {
  if (target.value === null) return null
  const planned = goToPlan({ status: panel.status, machineBlock: mdiBlock.value }, target.value.position, includeZ.value,
    panel.preferences.go_to_safe_z)
  return planned.ok ? planned.plan : planned.reason
})
const confirming = ref(false)

function openGoTo(entry: SavedPosition): void {
  if (!act(goToBlock.value)) return
  target.value = entry
  includeZ.value = true
  const planned = goTo.value
  if (typeof planned === 'string') { allow(planned); return }
  confirming.value = true
}

function startGoTo(): void {
  confirming.value = false
  const planned = goTo.value
  if (planned === null || typeof planned === 'string') { if (planned) allow(planned); return }
  if (!allow(goToBlock.value)) return
  sequence.start(planned.moves)
}

/** A machine coordinate with its sign, as the steps name it: "+169.870", "-50.000". */
const signed = (mm: number): string => `${mm >= 0 ? '+' : ''}${length(mm)}`

/** Each move of the plan in plain words: the lift to the safe height, X Y, Z down to the saved Z. */
const steps = computed(() => {
  const planned = goTo.value
  if (planned === null || typeof planned === 'string') return []
  return planned.moves.map((move) => {
    const { x, y, z } = move.target
    const text = x !== undefined && y !== undefined ? `Move X Y to X ${signed(x)}  Y ${signed(y)}`
      : z !== undefined && z === planned.safeZ && move === planned.moves[0]
        ? panel.preferences.go_to_safe_z === null ? `Lift Z to the top of travel (safe Z) ${signed(z)}` : `Lift Z to safe Z ${signed(z)}`
        : `Lower Z to the saved ${signed(z ?? 0)}`
    return { key: move.text, text }
  })
})
</script>

<template>
  <div class="positions">
    <div class="list">
      <table v-if="positions.length" aria-label="Saved machine positions">
        <thead>
          <tr>
            <th scope="col">Position</th>
            <th v-for="axis in ['X', 'Y', 'Z']" :key="axis" scope="col" class="axis-col">{{ axis }}</th>
            <th scope="col" class="go-col" aria-label="Move"></th>
            <th scope="col" class="delete-col" aria-label="Delete"></th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="entry in positions" :key="`${entry.name}:${entry.position.join(',')}`" class="row">
            <th scope="row" class="name">
              <button v-tooltip="saving ? 'Saving…' : `Rename ${entry.name}`" class="btn quiet rename" type="button" :aria-label="`Rename ${entry.name}`" :aria-disabled="saving" @click="openRename(entry)">
                <span class="text">{{ entry.name }}</span><AppIcon name="edit" />
              </button>
            </th>
            <td v-for="(value, index) in entry.position" :key="index" class="coord mono">{{ length(value) }}</td>
            <td class="go-col">
              <button v-tooltip="goToBlock ?? `Go to ${entry.name}`" class="btn" type="button" :aria-disabled="goToBlock !== null" @click="openGoTo(entry)">Go to</button>
            </td>
            <td class="delete-col">
              <button v-tooltip="saving ? 'Saving…' : `Delete ${entry.name}`" class="btn square quiet" type="button" :aria-label="`Delete ${entry.name}`" :aria-disabled="saving" @click="saving || (removal = entry)">
                <AppIcon name="trash" />
              </button>
            </td>
          </tr>
        </tbody>
      </table>
      <p v-else class="empty hint">No saved positions</p>
    </div>
    <div class="line">
      <button v-tooltip="addBlock ?? 'Save the present machine position'" class="btn" type="button" :aria-disabled="addBlock !== null" @click="openSave">
        <AppIcon name="plus" />Save position
      </button>
      <span class="hint">Machine coordinates · {{ unitName }}</span>
    </div>
  </div>

  <ModalDialog v-if="draft" :title="draft.renaming ? `Rename ${draft.renaming.name}` : 'Save position'" size="medium" expanded fresh-gesture @close="draft = null">
    <NameEntry v-model="draftName" label="Position name" :placeholder="draft.fallback" :max-length="maxPositionNameBytes" :replace-first="draft.renaming === null" @enter="confirmName" />
    <p class="coordinate mono">X {{ length(draft.position[0]) }}  Y {{ length(draft.position[1]) }}  Z {{ length(draft.position[2]) }} {{ unitName }}</p>
    <p v-if="nameBlock && nameBlock !== 'Saving…'" class="refusal" role="alert">{{ nameBlock }}</p>
    <p v-else>Machine coordinates, saved for this machine. An empty name saves as {{ draft.fallback }}.</p>
    <template #footer>
      <button class="btn wide" type="button" @click="draft = null">Cancel</button>
      <button v-tooltip="nameBlock ?? (draft.renaming ? 'Save the new name' : 'Save this position')" class="btn primary wide" type="button" :aria-disabled="nameBlock !== null" @click="confirmName">Save</button>
    </template>
  </ModalDialog>

  <ModalDialog v-if="removal" :title="`Delete ${removal.name}?`" size="small" fresh-gesture @close="removal = null">
    <p class="coordinate mono">X {{ length(removal.position[0]) }}  Y {{ length(removal.position[1]) }}  Z {{ length(removal.position[2]) }} {{ unitName }}</p>
    <template #footer>
      <button class="btn wide" type="button" @click="removal = null">Cancel</button>
      <button class="btn danger wide" type="button" @click="confirmDelete">Delete</button>
    </template>
  </ModalDialog>

  <ModalDialog v-if="confirming && target" :title="`Go to ${target.name}?`" size="medium" fresh-gesture @close="confirming = false">
    <ol v-if="steps.length > 0" class="steps">
      <li v-for="step in steps" :key="step.key" class="mono">{{ step.text }}</li>
    </ol>
    <p v-else-if="typeof goTo === 'string'" class="refusal">{{ goTo }}</p>
    <p v-else>The machine is already at this position.</p>
    <div class="z-choice">
      <span class="label">Z at the end</span>
      <div class="segmented" role="group" aria-label="Z at the end">
        <button class="btn" type="button" :class="{ on: !includeZ }" :aria-pressed="!includeZ" @click="includeZ = false">Stays at safe height</button>
        <button class="btn" type="button" :class="{ on: includeZ }" :aria-pressed="includeZ" @click="includeZ = true">Down to saved Z {{ length(target.position[2]) }}</button>
      </div>
    </div>
    <p>Rapid moves in machine coordinates, {{ unitName }}. Z first goes up to a safe height.</p>
    <template #footer>
      <button class="btn wide" type="button" @click="confirming = false">Cancel</button>
      <button class="btn primary wide" type="button" :aria-disabled="steps.length === 0" @click="steps.length > 0 && startGoTo()">Go to</button>
    </template>
  </ModalDialog>
</template>

<style scoped>
.positions { display: flex; flex-direction: column; gap: 6px; }
.list { height: 212px; overflow: auto; border: 1px solid var(--line); border-radius: var(--radius); background: var(--bg-sunken); }
.list table { width: 100%; table-layout: fixed; border-spacing: 0; text-align: left; }
.list thead { position: sticky; top: 0; background: var(--bg-sunken); }
.list thead th { padding: 6px 8px; border-bottom: 1px solid var(--line); color: var(--text-dim); font-size: 11px; font-weight: 600; }
.list .axis-col { width: 15%; text-align: right; }
.list .go-col { width: 70px; }
.list .delete-col { width: 48px; }
.row > * { padding: 2px 4px; }
.row:not(:last-child) > * { border-bottom: 1px solid var(--line); }
.row .name { padding: 2px 0 2px 2px; font-weight: 500; }
.rename { display: flex; justify-content: space-between; gap: 6px; width: 100%; padding-inline: 6px; font-weight: 500; text-align: left; }
.rename .text { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.rename :deep(svg) { flex: none; width: 16px; height: 16px; opacity: .6; }
.row .coord { padding-right: 8px; text-align: right; font-size: 12px; white-space: nowrap; }
.row .go-col .btn { width: 100%; padding-inline: 5px; }
.row .delete-col { padding-inline: 2px; }
.empty { padding: 12px; }
.line { display: flex; align-items: center; gap: 10px; }
.line .hint { margin-left: auto; font-size: 12px; }
.btn[aria-disabled="true"] { opacity: .68; }
.coordinate { margin: 0; font-size: 18px; white-space: pre; }
p { margin: 0; }
.steps { margin: 0; padding-left: 22px; display: flex; flex-direction: column; gap: 6px; font-size: 16px; }
.refusal { color: var(--stop); }
.z-choice { display: flex; flex-direction: column; gap: 6px; }
.z-choice .label { color: var(--text-dim); font-size: 12px; }
.z-choice .segmented { display: flex; }
.z-choice .segmented .btn { flex: 1; min-height: 48px; padding-inline: 8px; }
.wide { min-height: 48px; }
</style>
