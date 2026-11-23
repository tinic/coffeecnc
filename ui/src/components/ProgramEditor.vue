<script setup lang="ts">
// The program editor: the loaded program, or a new one, as plain text. A native textarea takes
// the input, so the system on-screen keyboard, selection and the caret work as anywhere else;
// the G-code colouring, line numbers and search marks are drawn behind and beside it, for the
// visible lines only, so a program of 512 KiB stays responsive.
import { computed, nextTick, onBeforeUnmount, onMounted, ref, shallowRef, useTemplateRef, watch } from 'vue'
import { useI18n } from 'vue-i18n'

import { onResult, panel, send } from '../machine'
import { connectionBlock } from '../machine-block'
import { tokenizeGcodeLine } from '../gcode-syntax'
import {
  byteLength, EditHistory, findAll, lineOf, lineStart, machineBusy, maxProgramBytes, programSource, uneditableReason,
  type EditorState, type EditorTarget,
} from '../program-editor'
import type { CommandBody, ProgramText } from '../protocol'
import { uniqueName } from '../conversational/operations'
import { baseName, formatBytes } from '../units'
import { vTooltip } from '../tooltip'
import AppIcon from './AppIcon.vue'
import ModalDialog from './ModalDialog.vue'
import { translateMessage } from '../i18n/messages'

const props = defineProps<{ target: EditorTarget }>()
const emit = defineEmits<{ close: []; newProgram: [folder: string] }>()
const { t } = useI18n()

// The program text the editor opened, once it matches the loaded program. It stays fixed while
// the editor is open: a save names that file and its stamp.
const loaded = shallowRef<ProgramText | null>(null)
const original = ref('')
const text = shallowRef('')
const folder = computed(() => loaded.value !== null ? loaded.value.file.slice(0, loaded.value.file.lastIndexOf('/')) || '/'
  : props.target.kind === 'new' ? props.target.folder : '')
const unavailable = computed(() => props.target.kind !== 'loaded' ? null
  : loaded.value === null ? t('program.editor.notArrived') : uneditableReason(loaded.value))
const dirty = computed(() => text.value !== original.value)
const running = computed(() => panel.status !== null && panel.status.interp !== 'idle')
const notice = computed(() => unavailable.value ?? (running.value ? t('program.editor.readOnlyRunning') : null))

// Saving under another name: always for a new program, on request for the loaded one.
const savingAs = ref(props.target.kind === 'new')
const name = ref(props.target.kind === 'new' ? uniqueName('program', new Set(props.target.taken)) : '')
const saving = ref(false)
const readOnly = computed(() => notice.value !== null || saving.value)
const feedback = ref('')
const size = computed(() => byteLength(text.value))
// Over the loaded program a save needs its stamp, and every save opens a program: neither while
// the machine is busy, as for the line edits.
const saveBlock = computed(() => connectionBlock(panel.link) ?? notice.value ??
  (!savingAs.value && loaded.value !== null && !loaded.value.stamp ? t('program.lineEdits.reopen') : null) ??
  (machineBusy(panel.status) ? t('program.editor.stopFirst') : null) ??
  (text.value.trim() === '' ? t('program.editor.empty') : null) ??
  (size.value > maxProgramBytes ? t('program.editor.tooLarge') : null) ??
  (savingAs.value && !/^[^/.][^/]*$/.test(name.value.trim()) ? t('program.editor.badName') : null))
const fileName = computed(() => {
  const typed = name.value.trim()
  return typed.includes('.') ? typed : `${typed}.ngc` // i18n-ignore
})
const title = computed(() => loaded.value !== null ? t('program.editor.editFile', { file: baseName(loaded.value.file) })
  : props.target.kind === 'new' ? t('program.editor.newProgram') : t('program.editor.editProgram'))

let saveRequest: CommandBody | null = null
let openRequest: CommandBody | null = null
function save(): void {
  if (saving.value || saveBlock.value !== null || (!dirty.value && !savingAs.value)) return
  feedback.value = ''
  saving.value = true
  const program = loaded.value
  saveRequest = savingAs.value || program === null
    ? { type: 'save_program', path: folder.value === '' ? fileName.value : `${folder.value === '/' ? '' : folder.value}/${fileName.value}`, text: text.value }
    : { type: 'save_program', path: program.file, text: text.value, stamp: program.stamp }
  send(saveRequest)
}
const removeListener = onResult((command, result) => {
  if (command === saveRequest) {
    saveRequest = null
    if (result.outcome !== 'accepted') {
      saving.value = false
      feedback.value = translateMessage(result.reason, result.refusal) || t('program.editor.notSaved')
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
    else feedback.value = t('program.editor.savedNotOpened', { reason: result.reason ? translateMessage(result.reason, result.refusal) : t('program.editor.refused') })
  }
})
onBeforeUnmount(removeListener)

// Closing, or starting a new program, with unsaved changes asks once.
const confirming = ref<'close' | 'new' | null>(null)
function leave(): void {
  if (confirming.value === 'new') emit('newProgram', folder.value)
  else emit('close')
}
function close(): void {
  if (saving.value || confirming.value !== null) return
  if (dirty.value) confirming.value = 'close'
  else emit('close')
}
function startNew(): void {
  if (dirty.value) confirming.value = 'new'
  else emit('newProgram', folder.value)
}

// The text area and what is drawn with it.
const area = useTemplateRef<HTMLTextAreaElement>('area')
const findField = useTemplateRef<HTMLInputElement>('findField')
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
const numberWidth = computed(() => `${String(Math.max(3, String(lines.value.length).length) + 2)}ch`) // i18n-ignore
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
  history.record(before ?? { text: text.value, ...selection }, event instanceof InputEvent ? event.inputType : '', Date.now(), element.selectionEnd)
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
    findField.value?.focus()
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
const matchLabel = computed(() => query.value === '' ? '' : matches.value.length === 0 ? t('program.editor.noMatch')
  : t('program.editor.matchOf', { index: matchIndex.value < 0 ? '–' : String(matchIndex.value + 1), count: matches.value.length }))
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
  name.value = uniqueName(`${baseName(program.file).replace(/\.[^.]*$/, '')}-copy`, new Set([baseName(program.file)])) // i18n-ignore
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
  <ModalDialog :title="title" size="wide" contained expanded docked :close-disabled="saving" @close="close">
    <template #header>
      <div class="editor-tools" role="toolbar" :aria-label="t('program.editor.toolbar')">
        <button v-tooltip="t('program.editor.undo')" class="btn square" type="button" :aria-label="t('program.editor.undo')" :disabled="!canUndo || readOnly" @click="undo"><AppIcon name="undo" /></button>
        <button v-tooltip="t('program.editor.redo')" class="btn square" type="button" :aria-label="t('program.editor.redo')" :disabled="!canRedo || readOnly" @click="redo"><AppIcon name="redo" /></button>
        <button v-tooltip="t('program.editor.newProgram')" class="btn square" type="button" :aria-label="t('program.editor.newProgram')" :disabled="saving" @click="startNew"><AppIcon name="plus" /></button>
        <span class="find">
          <AppIcon name="search" />
          <input ref="findField" v-model="query" class="field mono" type="search" :aria-label="t('program.editor.find')" :placeholder="t('program.editor.find')" autocomplete="off" spellcheck="false" @input="matchIndex = -1" @keydown.enter.prevent="findNext($event.shiftKey ? -1 : 1)" />
          <span class="count mono" role="status">{{ matchLabel }}</span>
        </span>
        <button v-tooltip="t('program.editor.previousMatch')" class="btn square" type="button" :aria-label="t('program.editor.previousMatch')" :disabled="matches.length === 0" @click="findNext(-1)"><AppIcon name="up" /></button>
        <button v-tooltip="t('program.editor.nextMatch')" class="btn square" type="button" :aria-label="t('program.editor.nextMatch')" :disabled="matches.length === 0" @click="findNext(1)"><AppIcon name="down" /></button>
        <span class="go">
          <input v-model="goLine" class="field mono" type="text" inputmode="numeric" :aria-label="t('program.editor.goToLine')" :placeholder="t('program.editor.line')" autocomplete="off" @keydown.enter.prevent="goTo" />
          <button class="btn" type="button" :disabled="goLine === ''" @click="goTo">{{ t('program.editor.go') }}</button>
        </span>
        <span class="where mono">{{ t('program.editor.where', { line: caretLine, lines: t('program.editor.lineCount', lineCount), size: formatBytes(size) }) }}</span>
      </div>
    </template>
    <div class="program-editor">
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
            ref="area" :value="text" class="area" :aria-label="t('program.editor.programText')" wrap="off" spellcheck="false" autocomplete="off" autocapitalize="off"
            :readonly="readOnly" @beforeinput="beforeInput" @input="input" @keydown="keydown" @scroll="sync" @select="sync" @keyup="sync" @pointerup="sync"
          ></textarea>
        </div>
      </div>
      <p v-if="feedback" class="feedback" role="alert">{{ translateMessage(feedback) }}</p>
    </div>
    <template #footer>
      <div class="file">
        <label v-if="savingAs" class="name">
          <span>{{ t('program.editor.saveAsLabel') }}</span>
          <input v-model="name" class="field mono" type="text" :aria-label="t('program.editor.fileName')" autocomplete="off" spellcheck="false" :disabled="saving" />
        </label>
        <template v-else-if="loaded">
          <strong class="mono">{{ baseName(loaded.file) }}</strong>
          <span v-tooltip="loaded.file" class="path">{{ folder }}</span>
        </template>
      </div>
      <span v-if="saveBlock && !notice" class="block">{{ saveBlock }}</span>
      <button class="btn" type="button" :disabled="saving" @click="close">{{ t('common.cancel') }}</button>
      <button v-if="loaded && !savingAs" class="btn" type="button" :disabled="saving || readOnly" @click="savingAs = true">{{ t('program.editor.saveAs') }}</button>
      <button v-if="loaded && savingAs" class="btn" type="button" :disabled="saving" @click="savingAs = false">{{ t('common.back') }}</button>
      <button v-tooltip="saveBlock ?? undefined" class="btn primary" type="button" :disabled="saving || saveBlock !== null || (!dirty && !savingAs)" @click="save">
        {{ saving ? t('common.saving') : savingAs || !loaded ? t('program.editor.saveAndOpen') : t('program.editor.saveAndReopen') }}
      </button>
    </template>
  </ModalDialog>
  <ModalDialog v-if="confirming" :title="t('program.editor.discardTitle')" size="small" expanded @close="confirming = null">
    <p class="confirm">{{ loaded ? t('program.editor.unsavedFile', { file: baseName(loaded.file) }) : t('program.editor.unsavedNew') }}</p>
    <template #footer>
      <button class="btn" type="button" @click="confirming = null">{{ t('program.editor.keepEditing') }}</button>
      <button class="btn primary" type="button" @click="leave">{{ t('program.editor.discard') }}</button>
    </template>
  </ModalDialog>
</template>

<style scoped>
.program-editor { display: flex; flex-direction: column; flex: 1; min-height: 0; }
.editor-tools { display: flex; align-items: center; gap: 6px; flex: 1; min-width: 0; }
.find { position: relative; display: flex; align-items: center; flex: 0 1 300px; min-width: 150px; margin-left: 8px; }
.find :deep(.icon) { position: absolute; left: 10px; width: 16px; height: 16px; color: var(--text-dim); pointer-events: none; }
.find input { width: 100%; padding-left: 34px; padding-right: 70px; }
.find .count { position: absolute; right: 10px; font-size: 11px; color: var(--text-dim); pointer-events: none; }
.go { display: flex; gap: 6px; margin-left: 8px; }
.go input { width: 84px; }
.where { margin-left: auto; font-size: 11px; color: var(--text-dim); white-space: nowrap; }
.notice, .feedback { display: flex; align-items: center; gap: 8px; margin: 0; padding: 6px 12px; font-size: 12px; flex: none; }
.notice { color: var(--warn); }
.notice :deep(.icon) { width: 15px; height: 15px; flex: none; }
.feedback { color: var(--stop); }

.editor {
  --code-padding: 12px;
  position: relative;
  display: flex;
  flex: 1;
  min-height: 0;
  border-block: 1px solid var(--line-strong);
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
@media (max-width: 1100px) { .where { display: none; } }
@media (max-width: 920px) { .editor-tools { order: 1; flex-basis: 100%; } }
</style>
