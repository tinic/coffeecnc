<script setup lang="ts">
import { vTooltip } from '../tooltip'
import { vPopover } from '../popover'
import type { LineAction } from '../program-line-edits'
// The program text. The executing line is highlighted and kept in view until the operator
// scrolls; from then on the view stays where it was put, and a button offers to follow again.
import { computed, nextTick, onBeforeUnmount, onMounted, ref, useTemplateRef, watch } from 'vue'

import { tokenizeGcodeLine } from '../gcode-syntax'
import AppIcon from './AppIcon.vue'

const props = defineProps<{
  lines: string[]
  /** Line whose motion is in progress, 1-based; 0 for none. */
  motionLine: number
  /** Line the interpreter is at; 0 for none. */
  currentLine: number
  /** Line of the preview's interpreter error; 0 for none. */
  errorLine: number
  /** Line numbers can be tapped to restart the program there. */
  restartable?: boolean
  breakpointLines?: ReadonlySet<number>
  breakpointLine?: number
  breakpointReached?: boolean
  breakpointBlock?: string | null
  edits?: {
    block: string | null
    hasClipboard: boolean
    canUndo: boolean
    message: { text: string; error: boolean } | null
  } | undefined
}>()
const emit = defineEmits<{
  restart: [line: number]
  editRate: [line: number, offset: number]
  breakpoint: [line: number]
  lineEdit: [action: LineAction, first: number, last: number]
  undoLineEdit: []
  dismissLineEdit: []
}>()

/** Where each token starts in its line, so a tapped F or S word can be found again. */
function offsetOf(tokens: readonly { text: string }[], index: number): number {
  return tokens.slice(0, index).reduce((sum, token) => sum + token.text.length, 0)
}

const anchor = ref(0)
const picked = ref(0)
const canEdit = computed(() => props.edits?.block === null)
const canPick = computed(() => props.restartable || canEdit.value)
const single = computed(() => picked.value > 0 && anchor.value === picked.value)
const selectionStart = computed(() => Math.min(anchor.value, picked.value))
const selectionEnd = computed(() => Math.max(anchor.value, picked.value))

function clearSelection(): void {
  anchor.value = picked.value = 0
}
watch(() => props.lines, clearSelection)
watch(canPick, (value) => { if (!value) clearSelection() })
function pick(line: number): void {
  if (!canPick.value) return
  if (single.value && picked.value === line) clearSelection()
  else anchor.value = picked.value = line
}
function pickCode(event: MouseEvent, line: number): void {
  if (event.target instanceof Element && event.target.classList.contains('editable')) return
  if (document.getSelection()?.isCollapsed === false) return
  pick(line)
}
function closeActions(): void {
  if (picked.value > 0) clearSelection()
  else emit('dismissLineEdit')
}
function editSelection(action: LineAction, event?: MouseEvent): void {
  if (!canEdit.value || picked.value === 0) return
  if (event?.currentTarget instanceof HTMLElement) event.currentTarget.closest('details')?.removeAttribute('open')
  emit('lineEdit', action, selectionStart.value, selectionEnd.value)
  if (action === 'copy') clearSelection()
}

const rowHeight = 24
const overscan = 8
const tabColumns = 8

const scroller = useTemplateRef<HTMLElement>('scroller')
const scrollTop = ref<number>(0)
const viewHeight = ref<number>(0)
const codeWidth = ref<number>(0)
let measureContext: CanvasRenderingContext2D | null = null
const follow = ref<boolean>(true)

// Capture only gutter gestures. Starting on the code remains native touch scrolling.
const dragging = ref(false)
let drag: { pointer: number; line: number; startY: number; y: number } | null = null
let scrollFrame = 0
function beginRange(event: PointerEvent, line: number): void {
  const element = scroller.value
  if (!canEdit.value || element === null || event.button !== 0 || drag !== null) return
  element.setPointerCapture(event.pointerId)
  drag = { pointer: event.pointerId, line, startY: event.clientY, y: event.clientY }
}
function updateRange(): void {
  const element = scroller.value
  if (drag === null || element === null) return
  const bounds = element.getBoundingClientRect()
  const edge = 24
  const edgeDelta = drag.y < bounds.top + edge ? drag.y - bounds.top - edge
    : drag.y > bounds.top + element.clientHeight - edge ? drag.y - bounds.top - element.clientHeight + edge : 0
  const previous = element.scrollTop
  if (edgeDelta !== 0) element.scrollTop += Math.sign(edgeDelta) * Math.min(14, Math.ceil(Math.abs(edgeDelta) / 3))
  onScroll()
  const y = Math.min(element.clientHeight - 1, Math.max(0, drag.y - bounds.top))
  anchor.value = drag.line
  picked.value = Math.min(props.lines.length, Math.floor((y + element.scrollTop) / rowHeight) + 1)
  scrollFrame = element.scrollTop !== previous ? requestAnimationFrame(updateRange) : 0
}
function moveRange(event: PointerEvent): void {
  if (drag?.pointer !== event.pointerId || !canEdit.value) return
  drag.y = event.clientY
  if (!dragging.value && Math.abs(drag.y - drag.startY) < 8) return
  dragging.value = true
  if (scrollFrame !== 0) cancelAnimationFrame(scrollFrame)
  updateRange()
}
function finishRange(event: PointerEvent): void {
  if (drag?.pointer !== event.pointerId) return
  const line = drag.line
  const moved = dragging.value
  cancelRange()
  if (event.type === 'pointerup' && !moved && canEdit.value) pick(line)
}
function cancelRange(): void {
  drag = null
  dragging.value = false
  if (scrollFrame !== 0) cancelAnimationFrame(scrollFrame)
  scrollFrame = 0
}
watch(canEdit, (value) => { if (!value && drag !== null) cancelRange() })

/** The line to keep in view: where motion is, or else where the interpreter is. */
const active = computed(() => props.breakpointReached && props.breakpointLine ? props.breakpointLine
  : props.motionLine > 0 ? props.motionLine : props.currentLine)

const first = computed(() => Math.max(0, Math.floor(scrollTop.value / rowHeight) - overscan))
const last = computed(() =>
  Math.min(props.lines.length, Math.ceil((scrollTop.value + viewHeight.value) / rowHeight) + overscan),
)
const rows = computed(() =>
  props.lines.slice(first.value, last.value).map((text, index) => ({
    number: first.value + index + 1,
    tokens: tokenizeGcodeLine(text),
  })),
)

// Measure the loaded text once per program or font change. Using only the rendered window
// introduces a horizontal scrollbar whenever a long line comes into view.
function measureCodeWidth(): void {
  if (scroller.value === null || measureContext === null) return
  const context = measureContext
  const style = getComputedStyle(scroller.value)
  context.font = `${style.fontSize} ${style.fontFamily}`
  context.fontKerning = 'none'
  const columnWidth = context.measureText('0').width
  const tabWidth = context.measureText(' ').width * tabColumns
  let widest = 0
  for (const line of props.lines) {
    if (/^[ -~]*$/.test(line)) {
      widest = Math.max(widest, line.length * columnWidth)
      continue
    }
    // Canvas treats tabs as spaces; the code pane displays them at the next tab stop.
    let width = 0
    const chunks = line.split('\t')
    for (const [index, chunk] of chunks.entries()) {
      if (index > 0) width = (Math.floor(width / tabWidth + 1e-8) + 1) * tabWidth
      width += context.measureText(chunk).width
    }
    widest = Math.max(widest, width)
  }
  codeWidth.value = Math.ceil(widest)
}

function show(line: number): void {
  const element = scroller.value
  if (element === null || line <= 0) return
  const center = Math.round((line - 0.5) * rowHeight - element.clientHeight / 2)
  const max = Math.max(0, element.scrollHeight - element.clientHeight)
  const target = Math.min(max, Math.max(0, center))
  if (element.scrollTop !== target) element.scrollTop = target
  scrollTop.value = element.scrollTop
}

/** Scrolls to `line`; this is the operator's choice, so following stops. */
function goTo(line: number): void {
  follow.value = false
  show(line)
}

defineExpose({ goTo })

function onScroll(): void {
  scrollTop.value = scroller.value?.scrollTop ?? 0
}

/** Wheel, touch, scroll bar or keys: the operator is reading, so the view stops following. */
function operatorScrolled(): void {
  if (active.value > 0) {
    follow.value = false
  }
}

function followAgain(): void {
  follow.value = true
  show(active.value)
}

watch(active, (line, before) => {
  if (line > 0 && before === 0) {
    // A new run starts in view.
    follow.value = true
  }
  if (follow.value && picked.value === 0) {
    show(line)
  }
})

watch(
  () => props.lines,
  () => {
    follow.value = true
    measureCodeWidth()
    void nextTick(() => {
      if (scroller.value !== null) {
        scroller.value.scrollTop = 0
        onScroll()
        updateViewport()
      }
    })
  },
)

function updateViewport(): void {
  const element = scroller.value
  if (element === null) return
  viewHeight.value = element.clientHeight
  if (dragging.value) return
  if (picked.value > 0) {
    const top = (picked.value - 1) * rowHeight
    const bottom = top + rowHeight
    // Scroll just enough to keep the row visible when the action bar takes space.
    const target = Math.min(top, Math.max(element.scrollTop, bottom - viewHeight.value))
    if (element.scrollTop !== target) element.scrollTop = target
    onScroll()
  } else if (follow.value) show(active.value)
}

watch(codeWidth, updateViewport, { flush: 'post' })
watch([picked, dragging], updateViewport, { flush: 'post' })

let observer: ResizeObserver | null = null

onMounted(() => {
  const element = scroller.value
  if (element === null) {
    return
  }
  measureContext = document.createElement('canvas').getContext('2d')
  measureCodeWidth()
  void document.fonts.ready.then(measureCodeWidth)
  observer = new ResizeObserver(updateViewport)
  observer.observe(element)
  updateViewport()
})

onBeforeUnmount(() => {
  cancelRange()
  observer?.disconnect()
})
</script>

<template>
  <div class="nc">
    <div
      ref="scroller"
      class="scroller gcode-tones"
      tabindex="0"
      @scroll="onScroll"
      @wheel.passive="operatorScrolled"
      @touchmove.passive="operatorScrolled"
      @pointerdown="operatorScrolled"
      @pointermove="moveRange"
      @pointerup="finishRange"
      @pointercancel="finishRange"
      @lostpointercapture="finishRange"
      @keydown="operatorScrolled"
    >
      <div
        class="spacer"
        :style="{ height: `${String(lines.length * rowHeight)}px`, '--code-width': `${String(codeWidth)}px` }"
      >
        <div class="rows" :style="{ transform: `translateY(${String(first * rowHeight)}px)` }">
          <div
            v-for="row in rows"
            :key="row.number"
            class="row"
            :class="{
              motion: row.number === motionLine,
              current: row.number === currentLine && row.number !== motionLine,
              error: row.number === errorLine,
              picked: row.number >= selectionStart && row.number <= selectionEnd,
              breakpoint: row.number === breakpointLine,
            }"
          >
            <span
              class="number" :class="{ pickable: canPick, 'range-target': canEdit }"
              @pointerdown="beginRange($event, row.number)" @click="(!canEdit || $event.detail === 0) && pick(row.number)"
            >{{ row.number }}</span>
            <span class="text" :class="{ pickable: canPick }" @click="pickCode($event, row.number)"><span
              v-for="(token, index) in row.tokens"
              :key="index"
              :data-tone="token.tone ?? undefined"
              :class="{ editable: restartable && single && row.number === picked && token.tone === 'rate' }"
              @click="restartable && single && row.number === picked && token.tone === 'rate' && emit('editRate', row.number, offsetOf(row.tokens, index))"
            >{{ token.text }}</span></span>
          </div>
        </div>
      </div>
    </div>
    <div v-if="!dragging && (picked > 0 || edits?.canUndo || edits?.message)" class="line-actions">
      <button v-tooltip="picked ? 'Clear selection' : 'Dismiss line actions'" class="btn clear-line" type="button" :aria-label="picked ? 'Clear selected line' : 'Dismiss line actions'" @click="closeActions">
        <AppIcon name="close" />
      </button>
      <span v-if="picked > 0 && !single" class="range-label mono">{{ selectionStart }}–{{ selectionEnd }}</span>
      <span v-if="edits?.message && (picked === 0 || edits.message.error)" class="edit-feedback" :class="{ 'edit-error': edits.message.error }" :role="edits.message.error ? 'alert' : 'status'">{{ edits.message.text }}</span>
      <button v-if="edits?.canUndo" v-tooltip="edits.block ?? 'Undo the last line edit'" class="btn undo-line" type="button" aria-label="Undo" :disabled="!!edits.block" @click="emit('undoLineEdit')">
        <AppIcon v-if="picked > 0" name="undo" /><span v-else>Undo</span>
      </button>
      <template v-if="picked > 0 && edits">
        <button v-if="!single" class="btn" type="button" :disabled="!!edits.block" @click="editSelection('cut')">Cut</button>
        <button v-if="!single" class="btn" type="button" :disabled="!!edits.block" @click="editSelection('copy')">Copy</button>
        <button v-tooltip="edits.block ?? (single ? 'Delete line' : 'Delete selected lines')" class="btn delete-line" type="button" :disabled="!!edits.block" :aria-label="single ? 'Delete line' : 'Delete selected lines'" @click="editSelection('delete')">
          <AppIcon name="trash" /> <span v-if="!single">Delete</span>
        </button>
        <details v-if="single && edits.hasClipboard && !edits.block" v-popover class="insert-lines">
          <summary class="btn">Insert</summary>
          <div class="popover insert-choices">
            <button class="btn" type="button" @click="editSelection('insert-above', $event)">Above line</button>
            <button class="btn" type="button" @click="editSelection('insert-below', $event)">Below line</button>
          </div>
        </details>
      </template>
      <template v-if="single && restartable">
        <button
          v-if="breakpointLines?.has(picked)" v-tooltip="breakpointBlock ?? 'Run from the beginning and pause before this line'"
          class="btn primary breakpoint-action" type="button" :aria-label="`Run to line ${String(picked)}`" :disabled="!!breakpointBlock" @click="emit('breakpoint', picked); clearSelection()"
        >
          <span>Run to<span class="full-label"> line</span></span> <span class="follow-count mono">{{ picked }}</span>
        </button>
        <button class="btn primary restart" type="button" :aria-label="`Restart from line ${String(picked)}`" @click="emit('restart', picked)">
          <span>Restart<span class="full-label"> from line</span></span> <span class="follow-count mono">{{ picked }}</span>
        </button>
      </template>
    </div>
    <button v-else-if="!follow && active > 0" class="btn follow" type="button" @click="followAgain">
      Follow line <span class="follow-count mono">{{ active }}</span>
    </button>
  </div>
</template>

<style scoped>
.nc {
  position: relative;
  container-type: inline-size;
  display: flex;
  flex-direction: column;
  min-width: 0;
  min-height: 0;
  border: 1px solid var(--line);
  border-radius: var(--radius);
  background: var(--bg-sunken);
  overflow: hidden;
}

.scroller {
  flex: 1;
  min-height: 0;
  overflow: auto;
  outline: none;
  overflow-anchor: none;
  --number-width: 54px;
  --code-padding: 12px;
  font-family: var(--mono);
  font-size: 14px;
  font-kerning: none;
  tab-size: 8;
}

.spacer {
  min-width: calc(var(--number-width) + var(--code-width) + var(--code-padding));
}

.row {
  display: flex;
  height: 24px;
  line-height: 24px;
  white-space: pre;
  user-select: text;
  -webkit-user-select: text;
}

.number {
  flex: none;
  width: var(--number-width);
  padding-right: 10px;
  color: var(--text-faint);
  text-align: right;
  user-select: none;
  -webkit-user-select: none;
}

.text {
  padding-right: var(--code-padding);
}

.row.current {
  background: var(--control);
}

.row.motion {
  background: var(--accent-soft);
  box-shadow: inset 3px 0 0 var(--accent);
}

.row.motion .number {
  color: var(--text);
}

.text .editable {
  cursor: pointer;
  text-decoration: underline;
  text-underline-offset: 3px;
}

.pickable { cursor: pointer; }
.number.range-target { touch-action: none; cursor: ns-resize; }

.row.picked {
  background: var(--accent-soft);
  box-shadow: inset 3px 0 0 var(--accent);
}

.row.error {
  background: var(--stop-soft);
  box-shadow: inset 3px 0 0 var(--stop);
}

.line-actions {
  display: flex;
  flex: none;
  flex-wrap: wrap;
  justify-content: flex-end;
  gap: 6px;
  padding: 6px 16px 10px;
  border-top: 1px solid var(--line);
}
.line-actions .btn { padding-inline: 10px; white-space: nowrap; }
.line-actions .clear-line, .line-actions .delete-line, .line-actions .undo-line { min-width: 44px; }
.line-actions .follow-count { width: auto; min-width: 1.5em; }
@container (max-width: 520px) { .line-actions .full-label { display: none; } }
.line-actions .clear-line { margin-right: auto; padding: 0; }
.range-label, .edit-feedback { align-self: center; color: var(--text-dim); font-size: 12px; }
.range-label + .edit-feedback { margin-left: auto; }
.edit-feedback { flex: 1; min-width: 60px; }
.edit-error { color: var(--stop); }
.insert-lines { position: relative; }
.insert-lines summary { list-style: none; }
.insert-lines summary::-webkit-details-marker { display: none; }
.insert-choices { display: grid; gap: 4px; width: max-content; bottom: calc(100% + 6px); padding: 6px; }
.row.breakpoint { background: var(--warn-soft); box-shadow: inset 3px 0 0 var(--warn); }
.row.breakpoint .number { color: var(--warn); }

.follow {
  position: absolute;
  right: 16px;
  bottom: 12px;
  border-color: var(--accent);
  background: var(--panel-raised);
}
.follow-count { width: 7ch; min-width: 7ch; text-align: right; }
</style>
