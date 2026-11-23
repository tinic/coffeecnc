<script setup lang="ts">
// The program editor: the loaded program, or a new one, as plain text. A native textarea takes
// the input, so the system on-screen keyboard, selection and the caret work as anywhere else;
// the G-code colouring, line numbers and search marks are drawn behind and beside it, for the
// visible lines only, so a program of 512 KiB stays responsive.
import { computed, nextTick, onBeforeUnmount, onMounted, ref, shallowRef, useTemplateRef, watch } from 'vue'

import { onResult, panel, send } from '../machine'
import { connectionBlock } from '../machine-block'
import { tokenizeGcodeLine } from '../gcode-syntax'
import {
  byteLength, EditHistory, findAll, lineOf, lineStart, maxProgramBytes, programSource, uneditableReason,
  type EditorState, type EditorTarget,
} from '../program-editor'
import type { CommandBody, ProgramText } from '../protocol'
import { uniqueName } from '../conversational/operations'
import { baseName, formatBytes } from '../units'
import { vTooltip } from '../tooltip'
import AppIcon from './AppIcon.vue'
import ModalDialog from './ModalDialog.vue'

const props = defineProps<{ target: EditorTarget }>()
const emit = defineEmits<{ close: [] }>()

// The program text the editor opened, once it matches the loaded program. It stays fixed while
// the editor is open: a save names that file and its stamp.
const loaded = shallowRef<ProgramText | null>(null)
const original = ref('')
const text = shallowRef('')
const folder = computed(() => loaded.value !== null ? loaded.value.file.slice(0, loaded.value.file.lastIndexOf('/')) || '/'
  : props.target.kind === 'new' ? props.target.folder : '')
const unavailable = computed(() => props.target.kind !== 'loaded' ? null
  : loaded.value === null ? 'The program text has not arrived yet.' : uneditableReason(loaded.value))
const dirty = computed(() => text.value !== original.value)
const running = computed(() => panel.status !== null && panel.status.interp !== 'idle')
const readOnly = computed(() => unavailable.value !== null || running.value)
const notice = computed(() => unavailable.value ?? (running.value ? 'Read-only while a program runs or is paused. Stop it to edit.' : null))

// Saving under another name: always for a new program, on request for the loaded one.
const savingAs = ref(props.target.kind === 'new')
const name = ref(props.target.kind === 'new' ? uniqueName('program', new Set(props.target.taken)) : '')
const saving = ref(false)
const feedback = ref('')
const size = computed(() => byteLength(text.value))
const saveBlock = computed(() => connectionBlock(panel.link) ?? notice.value ??
  (text.value.trim() === '' ? 'The program is empty.' : null) ??
  (size.value > maxProgramBytes ? 'The program is larger than 512 KiB.' : null) ??
  (savingAs.value && !/^[^/.][^/]*$/.test(name.value.trim()) ? 'Name the file without a slash or a leading dot.' : null))
const fileName = computed(() => {
  const typed = name.value.trim()
  return typed.includes('.') ? typed : `${typed}.ngc`
})
const title = computed(() => loaded.value !== null ? `Edit ${baseName(loaded.value.file)}`
  : props.target.kind === 'new' ? 'New program' : 'Edit program')

let saveRequest: CommandBody | null = null
let openRequest: CommandBody | null = null
function save(): void {
  if (saving.value || saveBlock.value !== null) return
  feedback.value = ''
  saving.value = true
  const program = loaded.value
  saveRequest = savingAs.value || program === null
    ? { type: 'save_program', path: `${folder.value === '/' ? '' : folder.value}/${fileName.value}`, text: text.value }
    : { type: 'save_program', path: program.file, text: text.value, stamp: program.stamp }
  send(saveRequest)
}
const removeListener = onResult((command, result) => {
  if (command === saveRequest) {
    saveRequest = null
    if (result.outcome !== 'accepted') {
      saving.value = false
      feedback.value = result.reason || 'The program was not saved.'
      return
    }
    // Over the loaded program the machine opened it again; a new file is opened now.
    if (command.type === 'save_program' && command.stamp === undefined) {
      openRequest = { type: 'open_program', path: command.path }
      send(openRequest)
    } else emit('close')
  } else if (command === openRequest) {
    openRequest = null
    saving.value = false
    if (result.outcome === 'accepted') emit('close')
    else feedback.value = `Saved, but not opened: ${result.reason || 'the machine refused it.'}`
  }
})
onBeforeUnmount(removeListener)

// Closing with unsaved changes asks once.
const confirming = ref(false)
function close(): void {
  if (dirty.value && !saving.value && !confirming.value) confirming.value = true
  else if (!confirming.value) emit('close')
}

// The text area and what is drawn with it.
const area = useTemplateRef<HTMLTextAreaElement>('area')
const rowHeight = 24
const overscan = 6
const scrollTop = ref(0)
const scrollLeft = ref(0)
const viewHeight = ref(0)
const caret = ref(0)
const lines = computed(() => text.value.split('\n'))
/** Lines as the code view counts them: a final line end starts no line. */
const lineCount = computed(() => text.value.endsWith('\n') ? lines.value.length - 1 : lines.value.length)
const starts = computed(() => {
  const found = [0]
  for (let at = text.value.indexOf('\n'); at >= 0; at = text.value.indexOf('\n', at + 1)) found.push(at + 1)
  return found
})
const first = computed(() => Math.max(0, Math.floor(scrollTop.value / rowHeight) - overscan))
const last = computed(() => Math.min(lines.value.length, Math.ceil((scrollTop.value + viewHeight.value) / rowHeight) + overscan))

const query = ref('')
const matches = computed(() => findAll(text.value, query.value))
const matchIndex = ref(-1)
function firstMatchFrom(offset: number): number {
  let low = 0
  let high = matches.value.length
  while (low < high) {
    const middle = (low + high) >> 1
    if ((matches.value[middle] ?? 0) < offset) low = middle + 1
    else high = middle
  }
  return low
}
interface Piece { text: string; mark: '' | 'match' | 'current' }
function marksOf(line: number, content: string): Piece[] {
  if (matches.value.length === 0) return []
  const start = starts.value[line - 1] ?? 0
  const length = query.value.length
  const pieces: Piece[] = []
  let at = 0
  for (let index = firstMatchFrom(start); index < matches.value.length; index += 1) {
    const offset = (matches.value[index] ?? 0) - start
    if (offset >= content.length) break
    pieces.push({ text: content.slice(at, offset), mark: '' })
    pieces.push({ text: content.slice(offset, offset + length), mark: index === matchIndex.value ? 'current' : 'match' })
    at = offset + length
  }
  return pieces
}
const rows = computed(() => lines.value.slice(first.value, last.value).map((content, index) => {
  const number = first.value + index + 1
  return { number, tokens: tokenizeGcodeLine(content), marks: marksOf(number, content) }
}))
const numberWidth = computed(() => `${String(Math.max(3, String(lines.value.length).length) + 2)}ch`)
const caretLine = computed(() => lineOf(text.value, caret.value))

function state(): EditorState {
  const element = area.value
  return { text: text.value, start: element?.selectionStart ?? 0, end: element?.selectionEnd ?? 0 }
}
// The selection as last seen: an edit is undone to the text and selection before it.
let selection = { start: 0, end: 0 }
function sync(): void {
  const element = area.value
  if (element === null) return
  scrollTop.value = element.scrollTop
  scrollLeft.value = element.scrollLeft
  viewHeight.value = element.clientHeight
  caret.value = element.selectionStart
  selection = { start: element.selectionStart, end: element.selectionEnd }
}

const history = new EditHistory()
const canUndo = ref(false)
const canRedo = ref(false)
function apply(next: EditorState | null): void {
  if (next === null) return
  text.value = next.text
  canUndo.value = history.canUndo
  canRedo.value = history.canRedo
  void nextTick(() => {
    const element = area.value
    if (element === null) return
    element.setSelectionRange(next.start, next.end)
    reveal(lineOf(next.text, next.start))
  })
}
function undo(): void { if (!readOnly.value) apply(history.undo(state())) }
function redo(): void { if (!readOnly.value) apply(history.redo(state())) }
function beforeInput(event: InputEvent): void {
  if (event.inputType === 'historyUndo' || event.inputType === 'historyRedo') {
    event.preventDefault()
    if (event.inputType === 'historyUndo') undo()
    else redo()
    return
  }
  before = state()
}
// Not every input announces itself (a composition, execCommand): the text is then the one
// before the input and the selection the last one seen.
let before: EditorState | null = null
function input(event: Event): void {
  const element = area.value
  if (element === null) return
  history.record(before ?? { text: text.value, ...selection }, event instanceof InputEvent ? event.inputType : '', Date.now())
  before = null
  text.value = element.value
  canUndo.value = history.canUndo
  canRedo.value = history.canRedo
  matchIndex.value = -1
  sync()
}
function keydown(event: KeyboardEvent): void {
  if (!(event.ctrlKey || event.metaKey) || event.altKey) return
  const key = event.key.toLowerCase()
  if (key === 'z') {
    event.preventDefault()
    if (event.shiftKey) redo()
    else undo()
  } else if (key === 'y') {
    event.preventDefault()
    redo()
  } else if (key === 'f') {
    event.preventDefault()
    document.querySelector<HTMLInputElement>('.editor-tools input[aria-label="Find"]')?.focus()
  } else if (key === 's') {
    event.preventDefault()
    save()
  }
}

/** Scrolls `line` to the middle of the view. */
function reveal(line: number): void {
  const element = area.value
  if (element === null) return
  element.scrollTop = Math.max(0, (line - 0.5) * rowHeight - element.clientHeight / 2)
  sync()
}
function select(start: number, end: number): void {
  const element = area.value
  if (element === null) return
  // Not focused: on the touch screen focus would raise the on-screen keyboard over the text.
  element.setSelectionRange(start, end)
  reveal(lineOf(text.value, start))
  caret.value = start
}
function findNext(step: 1 | -1): void {
  const count = matches.value.length
  if (count === 0) return
  if (matchIndex.value < 0) {
    const from = firstMatchFrom(area.value?.selectionEnd ?? 0)
    matchIndex.value = step === 1 ? from % count : (from - 1 + count) % count
  } else matchIndex.value = (matchIndex.value + step + count) % count
  const at = matches.value[matchIndex.value] ?? 0
  select(at, at + query.value.length)
}
const matchLabel = computed(() => query.value === '' ? '' : matches.value.length === 0 ? 'No match'
  : `${matchIndex.value < 0 ? '–' : String(matchIndex.value + 1)} of ${String(matches.value.length)}`)
const goLine = ref('')
function goTo(): void {
  const line = Number(goLine.value)
  if (!Number.isInteger(line) || line < 1) return
  const target = Math.min(line, lines.value.length)
  select(lineStart(text.value, target), lineStart(text.value, target))
}

// The loaded program's text, as soon as it matches the loaded program.
watch([(): ProgramText | null => panel.program, (): unknown => panel.status?.program], () => {
  const program = panel.program
  const loading = panel.status?.program
  if (props.target.kind !== 'loaded' || loaded.value !== null || program === null ||
      program.file !== loading?.file || program.revision !== loading.revision) return
  loaded.value = program
  original.value = programSource(program.lines)
  text.value = original.value
  name.value = uniqueName(`${baseName(program.file).replace(/\.[^.]*$/, '')}-copy`, new Set([baseName(program.file)]))
  void nextTick(() => {
    if (area.value === null) return
    area.value.setSelectionRange(0, 0)
    sync()
  })
}, { immediate: true })

let observer: ResizeObserver | null = null
onMounted(() => {
  const element = area.value
  if (element === null) return
  element.setSelectionRange(0, 0)
  sync()
  if (typeof ResizeObserver !== 'undefined') {
    observer = new ResizeObserver(sync)
    observer.observe(element)
  }
})
onBeforeUnmount(() => { observer?.disconnect() })
</script>

<template>
  <ModalDialog :title="title" size="wide" contained expanded @close="close">
    <div class="program-editor">
      <div class="editor-tools" role="toolbar" aria-label="Editor">
        <button v-tooltip="'Undo'" class="btn square" type="button" aria-label="Undo" :disabled="!canUndo || readOnly" @click="undo"><AppIcon name="undo" /></button>
        <button v-tooltip="'Redo'" class="btn square" type="button" aria-label="Redo" :disabled="!canRedo || readOnly" @click="redo"><AppIcon name="redo" /></button>
        <span class="find">
          <AppIcon name="search" />
          <input v-model="query" class="field mono" type="search" aria-label="Find" placeholder="Find" autocomplete="off" spellcheck="false" @input="matchIndex = -1" @keydown.enter.prevent="findNext($event.shiftKey ? -1 : 1)" />
          <span class="count mono" role="status">{{ matchLabel }}</span>
        </span>
        <button v-tooltip="'Previous match'" class="btn square" type="button" aria-label="Previous match" :disabled="matches.length === 0" @click="findNext(-1)"><AppIcon name="up" /></button>
        <button v-tooltip="'Next match'" class="btn square" type="button" aria-label="Next match" :disabled="matches.length === 0" @click="findNext(1)"><AppIcon name="down" /></button>
        <span class="go">
          <input v-model="goLine" class="field mono" type="text" inputmode="numeric" aria-label="Go to line" placeholder="Line" autocomplete="off" @keydown.enter.prevent="goTo" />
          <button class="btn" type="button" :disabled="goLine === ''" @click="goTo">Go</button>
        </span>
        <span class="where mono">Line {{ caretLine }} · {{ lineCount }} {{ lineCount === 1 ? 'line' : 'lines' }} · {{ formatBytes(size) }}</span>
      </div>
      <p v-if="notice" class="notice" role="status"><AppIcon name="info" />{{ notice }}</p>
      <div class="editor" :class="{ readonly: readOnly }" :style="{ '--number-width': numberWidth }">
        <div class="gutter mono" aria-hidden="true">
          <div class="rows" :style="{ transform: `translateY(${String(first * rowHeight - scrollTop)}px)` }">
            <div v-for="row in rows" :key="row.number" class="number" :class="{ caret: row.number === caretLine }">{{ row.number }}</div>
          </div>
        </div>
        <div class="code">
          <div class="layer marks" aria-hidden="true">
            <div class="rows" :style="{ transform: `translate(${String(-scrollLeft)}px, ${String(first * rowHeight - scrollTop)}px)` }">
              <div v-for="row in rows" :key="row.number" class="line"><span v-for="(piece, index) in row.marks" :key="index" :class="piece.mark">{{ piece.text }}</span></div>
            </div>
          </div>
          <div class="layer gcode-tones" aria-hidden="true">
            <div class="rows" :style="{ transform: `translate(${String(-scrollLeft)}px, ${String(first * rowHeight - scrollTop)}px)` }">
              <div v-for="row in rows" :key="row.number" class="line"><span v-for="(token, index) in row.tokens" :key="index" :data-tone="token.tone ?? undefined">{{ token.text }}</span></div>
            </div>
          </div>
          <textarea
            ref="area" :value="text" class="area" aria-label="Program text" wrap="off" spellcheck="false" autocomplete="off" autocapitalize="off"
            :readonly="readOnly" @beforeinput="beforeInput" @input="input" @keydown="keydown" @scroll="sync" @select="sync" @keyup="sync" @pointerup="sync"
          ></textarea>
        </div>
      </div>
      <p v-if="feedback" class="feedback" role="alert">{{ feedback }}</p>
    </div>
    <template #footer>
      <div class="file">
        <label v-if="savingAs" class="name">
          <span>Save as</span>
          <input v-model="name" class="field mono" type="text" aria-label="File name" autocomplete="off" spellcheck="false" :disabled="saving" />
        </label>
        <template v-else-if="loaded">
          <strong class="mono">{{ baseName(loaded.file) }}</strong>
          <span v-tooltip="loaded.file" class="path">{{ folder }}</span>
        </template>
      </div>
      <span v-if="saveBlock && !notice" class="block">{{ saveBlock }}</span>
      <button class="btn" type="button" @click="close">Cancel</button>
      <button v-if="loaded && !savingAs" class="btn" type="button" :disabled="saving || readOnly" @click="savingAs = true">Save as…</button>
      <button v-if="loaded && savingAs" class="btn" type="button" :disabled="saving" @click="savingAs = false">Back</button>
      <button v-tooltip="saveBlock ?? undefined" class="btn primary" type="button" :disabled="saving || saveBlock !== null || (!dirty && !savingAs)" @click="save">
        {{ saving ? 'Saving…' : savingAs || !loaded ? 'Save and open' : 'Save and reopen' }}
      </button>
    </template>
  </ModalDialog>
  <ModalDialog v-if="confirming" title="Discard changes?" size="small" expanded @close="confirming = false">
    <p class="confirm">The changes to {{ loaded ? baseName(loaded.file) : 'the new program' }} are not saved.</p>
    <template #footer>
      <button class="btn" type="button" @click="confirming = false">Keep editing</button>
      <button class="btn primary" type="button" @click="emit('close')">Discard changes</button>
    </template>
  </ModalDialog>
</template>

<style scoped>
.program-editor { display: flex; flex-direction: column; gap: 8px; flex: 1; min-height: 0; }
.editor-tools { display: flex; align-items: center; gap: 6px; flex: none; }
.find { position: relative; display: flex; align-items: center; flex: 0 1 300px; min-width: 150px; margin-left: 8px; }
.find :deep(.icon) { position: absolute; left: 10px; width: 16px; height: 16px; color: var(--text-dim); pointer-events: none; }
.find input { width: 100%; padding-left: 34px; padding-right: 70px; }
.find .count { position: absolute; right: 10px; font-size: 11px; color: var(--text-dim); pointer-events: none; }
.go { display: flex; gap: 6px; margin-left: 8px; }
.go input { width: 84px; }
.where { margin-left: auto; font-size: 11px; color: var(--text-dim); white-space: nowrap; }
.notice, .feedback { display: flex; align-items: center; gap: 8px; margin: 0; font-size: 12px; flex: none; }
.notice { color: var(--warn); }
.notice :deep(.icon) { width: 15px; height: 15px; flex: none; }
.feedback { color: var(--stop); }

.editor {
  --code-padding: 12px;
  position: relative;
  display: flex;
  flex: 1;
  min-height: 0;
  border: 1px solid var(--line-strong);
  border-radius: var(--radius);
  background: var(--bg-sunken);
  overflow: hidden;
  font-family: var(--mono);
  font-size: 14px;
  font-kerning: none;
  line-height: 24px;
  tab-size: 8;
}
.editor:focus-within { border-color: var(--accent); }
.gutter { position: relative; flex: none; width: var(--number-width); overflow: hidden; border-right: 1px solid var(--line); color: var(--text-faint); text-align: right; user-select: none; -webkit-user-select: none; }
.gutter .number { height: 24px; padding-right: 10px; }
.gutter .number.caret { color: var(--text); }
.code { position: relative; flex: 1; min-width: 0; }
.layer { position: absolute; inset: 0; overflow: hidden; pointer-events: none; }
.rows { will-change: transform; }
.line { height: 24px; padding: 0 var(--code-padding); white-space: pre; }
/* A synthesised bold is wider than the textarea's glyphs: every tone keeps its weight. */
.layer [data-tone] { font-weight: inherit; }
.marks .line { color: transparent; }
.marks .match { border-radius: 3px; background: var(--warn-soft); }
.marks .current { border-radius: 3px; background: color-mix(in srgb, var(--warn) 45%, transparent); }
.area {
  position: absolute;
  inset: 0;
  width: 100%;
  height: 100%;
  margin: 0;
  padding: 0 var(--code-padding);
  border: 0;
  outline: none;
  resize: none;
  overflow: auto;
  background: transparent;
  color: transparent;
  -webkit-text-fill-color: transparent;
  caret-color: var(--text);
  font: inherit;
  line-height: inherit;
  tab-size: inherit;
  white-space: pre;
  overflow-wrap: normal;
  user-select: text;
  -webkit-user-select: text;
}
.area::selection { background: color-mix(in srgb, var(--accent) 30%, transparent); }
.readonly .area { caret-color: transparent; }

.file { flex: 1; min-width: 0; overflow: hidden; align-self: center; font-size: 12px; }
.file strong, .path { display: block; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.file strong { font-size: 13px; font-weight: 500; }
.path { margin-top: 2px; color: var(--text-dim); }
.name { display: flex; align-items: center; gap: 10px; }
.name span { color: var(--text-dim); white-space: nowrap; }
.name input { flex: 1; min-width: 0; max-width: 360px; }
.block { align-self: center; max-width: 280px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; font-size: 12px; color: var(--text-dim); }
.confirm { margin: 0; }
@media (max-width: 1000px) { .where { display: none; } }
</style>
