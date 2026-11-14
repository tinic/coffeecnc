<script setup lang="ts">
// The program text. The executing line is highlighted and kept in view until the operator
// scrolls; from then on the view stays where it was put, and a button offers to follow again.
import { computed, nextTick, onBeforeUnmount, onMounted, ref, useTemplateRef, watch } from 'vue'

const props = defineProps<{
  lines: string[]
  /** Line whose motion is in progress, 1-based; 0 for none. */
  motionLine: number
  /** Line the interpreter is at; 0 for none. */
  currentLine: number
  /** Line of the preview's interpreter error; 0 for none. */
  errorLine: number
}>()

const rowHeight = 24
const overscan = 8

const scroller = useTemplateRef<HTMLElement>('scroller')
const scrollTop = ref<number>(0)
const viewHeight = ref<number>(0)
const follow = ref<boolean>(true)

/** The line to keep in view: where motion is, or else where the interpreter is. */
const active = computed(() => (props.motionLine > 0 ? props.motionLine : props.currentLine))

const first = computed(() => Math.max(0, Math.floor(scrollTop.value / rowHeight) - overscan))
const last = computed(() =>
  Math.min(props.lines.length, Math.ceil((scrollTop.value + viewHeight.value) / rowHeight) + overscan),
)
const rows = computed(() =>
  props.lines.slice(first.value, last.value).map((text, index) => ({ number: first.value + index + 1, text })),
)

function isComment(text: string): boolean {
  const trimmed = text.trimStart()
  return trimmed.startsWith('(') || trimmed.startsWith(';')
}

function show(line: number): void {
  const element = scroller.value
  if (element !== null && line > 0) {
    element.scrollTop = Math.max(0, (line - 0.5) * rowHeight - element.clientHeight / 2)
  }
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
  if (follow.value) {
    show(line)
  }
})

watch(
  () => props.lines,
  () => {
    follow.value = true
    void nextTick(() => {
      if (scroller.value !== null) {
        scroller.value.scrollTop = 0
        show(active.value)
      }
    })
  },
)

let observer: ResizeObserver | null = null

onMounted(() => {
  const element = scroller.value
  if (element === null) {
    return
  }
  viewHeight.value = element.clientHeight
  observer = new ResizeObserver(() => {
    viewHeight.value = element.clientHeight
    if (follow.value) {
      show(active.value)
    }
  })
  observer.observe(element)
  show(active.value)
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
      <div class="spacer" :style="{ height: `${String(lines.length * rowHeight)}px` }">
        <div class="rows" :style="{ transform: `translateY(${String(first * rowHeight)}px)` }">
          <div
            v-for="row in rows"
            :key="row.number"
            class="row"
            :class="{
              motion: row.number === motionLine,
              current: row.number === currentLine && row.number !== motionLine,
              error: row.number === errorLine,
              comment: isComment(row.text),
            }"
          >
            <span class="number">{{ row.number }}</span>
            <span class="text">{{ row.text }}</span>
          </div>
        </div>
      </div>
    </div>
    <button v-if="!follow && active > 0" class="btn follow" type="button" @click="followAgain">
      Follow line {{ active }}
    </button>
  </div>
</template>

<style scoped>
.nc {
  position: relative;
  min-width: 0;
  min-height: 0;
  border: 1px solid var(--line);
  border-radius: var(--radius);
  background: var(--bg-sunken);
  overflow: hidden;
}

.scroller {
  height: 100%;
  overflow: auto;
  outline: none;
}

.spacer {
  min-width: max-content;
}

.row {
  display: flex;
  height: 24px;
  font-family: var(--mono);
  font-size: 14px;
  line-height: 24px;
  white-space: pre;
  user-select: text;
  -webkit-user-select: text;
}

.number {
  flex: none;
  width: 54px;
  padding-right: 10px;
  color: var(--text-faint);
  text-align: right;
  user-select: none;
  -webkit-user-select: none;
}

.text {
  padding-right: 12px;
}

.row.comment .text {
  color: var(--text-dim);
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

.row.error {
  background: var(--stop-soft);
  box-shadow: inset 3px 0 0 var(--stop);
}

.follow {
  position: absolute;
  right: 16px;
  bottom: 12px;
  border-color: var(--accent);
  background: var(--panel-raised);
}
</style>
