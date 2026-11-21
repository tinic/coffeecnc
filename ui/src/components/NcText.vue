<script setup lang="ts">
import { vTooltip } from '../tooltip'
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
}>()
const emit = defineEmits<{ restart: [line: number]; editRate: [line: number, offset: number]; breakpoint: [line: number] }>()

/** Where each token starts in its line, so a tapped F or S word can be found again. */
function offsetOf(tokens: readonly { text: string }[], index: number): number {
  return tokens.slice(0, index).reduce((sum, token) => sum + token.text.length, 0)
}

/** The line the operator tapped to restart from; 0 for none. */
const picked = ref<number>(0)
watch(() => [props.restartable, props.lines] as const, () => { picked.value = 0 })
function pick(line: number): void {
  if (!props.restartable) return
  picked.value = picked.value === line ? 0 : line
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
watch(picked, updateViewport, { flush: 'post' })

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
  observer?.disconnect()
})
</script>

<template>
  <div class="nc">
    <div
      ref="scroller"
      class="scroller"
      tabindex="0"
      @scroll="onScroll"
      @wheel.passive="operatorScrolled"
      @touchmove.passive="operatorScrolled"
      @pointerdown="operatorScrolled"
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
              picked: row.number === picked,
              breakpoint: row.number === breakpointLine,
            }"
          >
            <span class="number" :class="{ pickable: restartable }" @click="pick(row.number)">{{ row.number }}</span>
            <span class="text"><span
              v-for="(token, index) in row.tokens"
              :key="index"
              :data-tone="token.tone ?? undefined"
              :class="{ editable: restartable && row.number === picked && token.tone === 'rate' }"
              @click="restartable && row.number === picked && token.tone === 'rate' && emit('editRate', row.number, offsetOf(row.tokens, index))"
            >{{ token.text }}</span></span>
          </div>
        </div>
      </div>
    </div>
    <div v-if="restartable && picked > 0" class="line-actions">
      <button class="btn clear-line" type="button" aria-label="Clear selected line" @click="picked = 0">
        <AppIcon name="close" />
      </button>
      <button
        v-if="breakpointLines?.has(picked)" v-tooltip="breakpointBlock ?? 'Run from the beginning and pause before this line'"
        class="btn primary breakpoint-action" type="button" :disabled="!!breakpointBlock" @click="emit('breakpoint', picked); picked = 0"
      >
        Run to line <span class="follow-count mono">{{ picked }}</span>
      </button>
      <button class="btn primary restart" type="button" @click="emit('restart', picked)">
        Restart from line <span class="follow-count mono">{{ picked }}</span>
      </button>
    </div>
    <button v-else-if="!follow && active > 0" class="btn follow" type="button" @click="followAgain">
      Follow line <span class="follow-count mono">{{ active }}</span>
    </button>
  </div>
</template>

<style scoped>
.nc {
  position: relative;
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

[data-tone='rapid'] { color: var(--gcode-rapid); }
[data-tone='cut'] { color: var(--gcode-cut); }
[data-tone='modal'] { color: var(--gcode-modal); }
[data-tone='x'] { color: var(--gcode-x); }
[data-tone='y'] { color: var(--gcode-y); }
[data-tone='z'] { color: var(--gcode-z); }
[data-tone='argument'] { color: var(--gcode-argument); }
[data-tone='rate'] { color: var(--gcode-rate); }
[data-tone='tool'] { color: var(--gcode-tool); }
[data-tone='spindle'] { color: var(--gcode-spindle); }
[data-tone='stop'] { color: var(--gcode-stop); font-weight: 700; }
[data-tone='comment'] { color: var(--gcode-comment); font-style: italic; }
[data-tone='message'] { color: var(--gcode-message); font-style: italic; font-weight: 500; }
[data-tone='flow'] { color: var(--gcode-flow); }
[data-tone='parameter'] { color: var(--gcode-parameter); }
[data-tone='expression'] { color: var(--gcode-expression); }

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

.number.pickable {
  cursor: pointer;
}

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
.clear-line { margin-right: auto; min-width: 44px; padding: 0; }
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
