<script setup lang="ts">
// Manual workspace: homing, jogging, MDI, spindle, coolant and work system selection.
//
// Jogging follows jogHold.ts: a jog runs only while a finger, mouse button or key is known to be
// down, and the keyboard jogs only while this workspace is shown, no dialog is open and no field
// has the focus.
import { computed, nextTick, onBeforeUnmount, reactive, ref, useTemplateRef, watch } from 'vue'

import { openDialogs } from '../dialogs'
import { JogHold, Keys, Touches, capturedPress } from '../jogHold'
import { axes, manualBlock, mdiBlock, onResult, panel, send } from '../machine'
import type { AxisLetter } from '../protocol'
import { formatIncrement, formatVelocity, parseNumber, velocityUnit, wcsNames } from '../units'
import AppIcon from './AppIcon.vue'

type JogKey = 'x+' | 'x-' | 'y+' | 'y-' | 'z+' | 'z-'

const jogs: Readonly<Record<JogKey, { axis: AxisLetter; direction: -1 | 1 }>> = {
  'x+': { axis: 'x', direction: 1 },
  'x-': { axis: 'x', direction: -1 },
  'y+': { axis: 'y', direction: 1 },
  'y-': { axis: 'y', direction: -1 },
  'z+': { axis: 'z', direction: 1 },
  'z-': { axis: 'z', direction: -1 },
}

const keyboard: Readonly<Record<string, JogKey>> = {
  ArrowRight: 'x+',
  ArrowLeft: 'x-',
  ArrowUp: 'y+',
  ArrowDown: 'y-',
  PageUp: 'z+',
  PageDown: 'z-',
}

const status = computed(() => panel.status)
const unit = computed(() => panel.unit)
const held = reactive(new Set<JogKey>())

const jogHold = new JogHold<JogKey>(
  {
    send: (key: JogKey, isHeld: boolean): void => {
      if (isHeld) {
        held.add(key)
      } else {
        held.delete(key)
      }
      send({ type: 'jog', ...jogs[key], velocity: panel.jogVelocity, held: isHeld })
    },
  },
  {
    setInterval: (callback: () => void, ms: number): number => window.setInterval(callback, ms),
    clearInterval: (handle: number): void => {
      window.clearInterval(handle)
    },
  },
)
const touches = new Touches()
const keys = new Keys()

/** A press on a jog control: a held jog, or one step when an increment is selected. */
function begin(key: JogKey, stillHeld: () => boolean): void {
  if (manualBlock.value !== null) {
    return
  }
  const increment = panel.jogIncrement
  if (increment === null) {
    jogHold.press(key, stillHeld)
  } else {
    const jog = jogs[key]
    send({ type: 'jog_step', axis: jog.axis, distance: jog.direction * increment, velocity: panel.jogVelocity })
  }
}

function jogPointerDown(key: JogKey, event: PointerEvent): void {
  // Touch is handled by the touch events; mouse events right after a touch are WebKit's emulation.
  if (event.pointerType !== 'mouse' || event.button !== 0 || touches.emulatingMouse(event.timeStamp)) {
    return
  }
  const stillHeld = capturedPress(event.currentTarget as HTMLElement, event.pointerId)
  if (stillHeld !== null) {
    begin(key, stillHeld)
  }
}

function jogTouchStart(key: JogKey, event: TouchEvent): void {
  const touch = event.changedTouches.item(0)
  if (touch !== null) {
    begin(key, touches.start(touch.identifier, event.timeStamp))
  }
}

function touchesEnded(event: TouchEvent): void {
  for (const touch of Array.from(event.changedTouches)) {
    touches.end(touch.identifier, event.timeStamp)
  }
}

function jogTouchEnd(key: JogKey, event: TouchEvent): void {
  touchesEnded(event)
  jogHold.release(key)
}

function releaseAll(): void {
  jogHold.releaseAll()
  keys.clear()
}

function stopJog(): void {
  releaseAll()
  send({ type: 'jog_stop_all' })
}

function stopTouch(event: TouchEvent): void {
  touches.touched(event.timeStamp)
  stopJog()
}

function stopPointerDown(event: PointerEvent): void {
  if (event.pointerType === 'mouse' && !touches.emulatingMouse(event.timeStamp)) {
    stopJog()
  }
}

function typing(): boolean {
  const element = document.activeElement
  return (
    element instanceof HTMLInputElement ||
    element instanceof HTMLTextAreaElement ||
    element instanceof HTMLSelectElement ||
    (element instanceof HTMLElement && element.isContentEditable)
  )
}

function keyDown(event: KeyboardEvent): void {
  const key = keyboard[event.code]
  if (key === undefined || typing() || openDialogs.value > 0 || event.ctrlKey || event.altKey || event.metaKey) {
    return
  }
  event.preventDefault()
  if (!event.repeat) {
    begin(key, keys.press(event.code))
  }
}

function keyUp(event: KeyboardEvent): void {
  const key = keyboard[event.code]
  keys.release(event.code)
  if (key !== undefined) {
    jogHold.release(key)
  }
}

/** The panel lost the keyboard or the screen: no release will arrive, so everything stops. */
function lostAttention(): void {
  if (jogHold.any) {
    stopJog()
  } else {
    releaseAll()
  }
}

function focusIn(): void {
  if (typing()) {
    lostAttention()
  }
}

function visibilityChanged(): void {
  if (document.visibilityState !== 'visible') {
    lostAttention()
  }
}

// A touch that ends anywhere (finger slid off the button, or the end event went elsewhere) ends
// its jog: the next refresh sees the touch is no longer down.
window.addEventListener('touchend', touchesEnded, true)
window.addEventListener('touchcancel', touchesEnded, true)
window.addEventListener('keydown', keyDown)
window.addEventListener('keyup', keyUp)
window.addEventListener('blur', lostAttention)
window.addEventListener('focusin', focusIn)
document.addEventListener('visibilitychange', visibilityChanged)

// A jog the machine refuses is not refreshed twenty times a second.
const removeResultListener = onResult((command, result) => {
  if (command.type === 'jog' && command.held && result.outcome !== 'accepted') {
    jogHold.release(`${command.axis}${command.direction > 0 ? '+' : '-'}`)
  }
})

// A dialog opening ends keyboard jogging, as a field taking the focus does.
watch(openDialogs, () => {
  lostAttention()
})

onBeforeUnmount(() => {
  window.removeEventListener('touchend', touchesEnded, true)
  window.removeEventListener('touchcancel', touchesEnded, true)
  window.removeEventListener('keydown', keyDown)
  window.removeEventListener('keyup', keyUp)
  window.removeEventListener('blur', lostAttention)
  window.removeEventListener('focusin', focusIn)
  document.removeEventListener('visibilitychange', visibilityChanged)
  removeResultListener()
  lostAttention()
})

function setJogVelocity(event: Event): void {
  panel.jogVelocity = Number((event.target as HTMLInputElement).value)
}

function blur(event: Event): void {
  // Leave the slider, so that the arrow keys jog again.
  ;(event.target as HTMLElement).blur()
}

const pad: readonly (readonly [JogKey, string, 'up' | 'down' | 'left' | 'right'])[] = [
  ['y+', 'ypos', 'up'],
  ['x-', 'xneg', 'left'],
  ['x+', 'xpos', 'right'],
  ['y-', 'yneg', 'down'],
  ['z+', 'zpos', 'up'],
  ['z-', 'zneg', 'down'],
]

function jogLabel(key: JogKey): string {
  return `${key.charAt(0).toUpperCase()}${key.charAt(1) === '+' ? '+' : '−'}`
}

// ---- homing ---------------------------------------------------------------------------------------

const homeBlock = manualBlock

// ---- MDI ------------------------------------------------------------------------------------------

const mdiText = ref<string>('')
const mdiList = useTemplateRef<HTMLElement>('mdiList')
let recall = -1

function sendMdi(): void {
  const text = mdiText.value.trim()
  if (text === '' || mdiBlock.value !== null) {
    return
  }
  send({ type: 'mdi', text })
  mdiText.value = ''
  recall = -1
}

function recallMdi(step: -1 | 1): void {
  const history = panel.mdi
  if (history.length === 0) {
    return
  }
  recall = recall < 0 ? (step < 0 ? history.length - 1 : -1) : recall + step
  if (recall < 0 || recall >= history.length) {
    recall = -1
    mdiText.value = ''
    return
  }
  mdiText.value = history[recall]?.text ?? ''
}

watch(
  () => panel.mdi.length,
  () => {
    void nextTick(() => {
      mdiList.value?.scrollTo({ top: mdiList.value.scrollHeight })
    })
  },
  { immediate: true },
)

// ---- spindle and coolant ------------------------------------------------------------------------------

const rpmText = ref<string>('')
const rpm = computed(() => {
  if (rpmText.value.trim() === '') {
    return status.value?.spindle.commanded_rpm ?? 0
  }
  return parseNumber(rpmText.value)
})
const spindleBlock = computed((): string | null => {
  if (manualBlock.value !== null) {
    return manualBlock.value
  }
  return rpm.value === null || rpm.value <= 0 ? 'Enter a spindle speed above zero' : null
})

function spindle(direction: 'forward' | 'reverse'): void {
  if (rpm.value !== null) {
    send({ type: 'set_spindle', direction, rpm: rpm.value })
  }
}

const coolantBlock = computed((): string | null => {
  const s = status.value
  if (s === null || panel.link !== 'live') {
    return 'No live status'
  }
  return s.machine === 'on' ? null : 'Machine is not on'
})

function coolant(which: 'flood' | 'mist'): void {
  const now = status.value?.coolant ?? { flood: false, mist: false }
  send({
    type: 'set_coolant',
    flood: which === 'flood' ? !now.flood : now.flood,
    mist: which === 'mist' ? !now.mist : now.mist,
  })
}
</script>

<template>
  <div class="manual">
    <section class="card jog">
      <h2>Jog</h2>
      <p v-if="manualBlock" class="blocked">Jog and homing unavailable: {{ manualBlock }}</p>
      <div class="pads">
        <div class="pad">
          <button
            v-for="[key, area, icon] in pad"
            :key="key"
            class="btn jogbtn"
            type="button"
            :class="{ held: held.has(key) }"
            :style="{ gridArea: area }"
            :disabled="manualBlock !== null"
            :title="manualBlock ?? `Jog ${jogLabel(key)}`"
            @touchstart.prevent="jogTouchStart(key, $event)"
            @touchend.prevent="jogTouchEnd(key, $event)"
            @touchcancel="jogTouchEnd(key, $event)"
            @pointerdown="jogPointerDown(key, $event)"
            @pointerup="jogHold.release(key)"
            @pointercancel="jogHold.release(key)"
            @lostpointercapture="jogHold.release(key)"
            @contextmenu.prevent
          >
            <AppIcon :name="icon" />
            <span class="mono">{{ jogLabel(key) }}</span>
          </button>
          <button
            class="btn danger jogbtn stopjog"
            type="button"
            title="Stop all jogging"
            @touchstart.prevent="stopTouch"
            @pointerdown="stopPointerDown"
          >
            Stop jog
          </button>
        </div>
      </div>

      <div class="line">
        <span class="label">Step</span>
        <div class="segmented">
          <button
            class="btn"
            type="button"
            :class="{ on: panel.jogIncrement === null }"
            title="Jog while the button or key is held"
            @click="panel.jogIncrement = null"
          >
            Continuous
          </button>
          <button
            v-for="increment in panel.hello?.jog_increments ?? []"
            :key="increment"
            class="btn mono"
            type="button"
            :class="{ on: panel.jogIncrement === increment }"
            @click="panel.jogIncrement = increment"
          >
            {{ formatIncrement(increment, unit) }}
          </button>
        </div>
        <span class="hint">{{ unit }}</span>
      </div>

      <div class="line">
        <span class="label">Speed</span>
        <input
          type="range"
          min="10"
          :max="panel.hello?.max_jog_velocity ?? 10"
          step="10"
          :value="panel.jogVelocity"
          aria-label="Jog speed"
          @input="setJogVelocity"
          @change="blur"
        />
        <span class="speed mono">{{ formatVelocity(panel.jogVelocity, unit) }}</span>
        <span class="hint">{{ velocityUnit(unit) }}</span>
      </div>

      <div class="line">
        <span class="label">Home</span>
        <button
          class="btn"
          type="button"
          :disabled="homeBlock !== null"
          :title="homeBlock ?? 'Home all axes, Z first'"
          @click="send({ type: 'home', axis: 'all' })"
        >
          <AppIcon name="home" />All
        </button>
        <button
          v-for="axis in axes"
          :key="axis.axis"
          class="btn homeaxis"
          type="button"
          :disabled="homeBlock !== null"
          :title="homeBlock ?? `Home ${axis.axis.toUpperCase()}`"
          @click="send({ type: 'home', axis: axis.axis })"
        >
          {{ axis.axis.toUpperCase() }}
          <span class="homestate" :class="{ homed: axis.homed }">{{
            axis.homing ? 'homing' : axis.homed ? 'homed' : 'not homed'
          }}</span>
        </button>
      </div>
      <p class="hint keys">
        Keyboard: arrow keys jog X and Y, Page Up and Page Down jog Z, while this workspace is shown and no field has
        the focus.
      </p>
    </section>

    <div class="side">
      <section class="card mdi">
        <h2>MDI</h2>
        <div ref="mdiList" class="history">
          <p v-if="panel.mdi.length === 0" class="hint">No commands sent in this session.</p>
          <button
            v-for="entry in panel.mdi"
            :key="entry.id"
            class="entry"
            type="button"
            :class="entry.outcome"
            title="Copy to the entry field"
            @click="mdiText = entry.text"
          >
            <span class="mono text">{{ entry.text }}</span>
            <span class="result">
              <template v-if="entry.outcome === 'pending'">sent</template>
              <template v-else-if="entry.outcome === 'accepted'">accepted</template>
              <template v-else>{{ entry.reason }}</template>
            </span>
          </button>
        </div>
        <form class="line" @submit.prevent="sendMdi">
          <input
            v-model="mdiText"
            class="field mono grow"
            type="text"
            placeholder="G-code, for example G0 X0 Y0"
            autocomplete="off"
            autocapitalize="characters"
            spellcheck="false"
            aria-label="MDI command"
            @keydown.up.prevent="recallMdi(-1)"
            @keydown.down.prevent="recallMdi(1)"
          />
          <button class="btn primary" type="submit" :disabled="mdiBlock !== null || mdiText.trim() === ''" :title="mdiBlock ?? 'Run this command'">
            Run
          </button>
        </form>
        <p v-if="mdiBlock" class="blocked">MDI unavailable: {{ mdiBlock }}</p>
      </section>

      <section class="card">
        <h2>Spindle and coolant</h2>
        <div class="line">
          <input
            v-model="rpmText"
            class="field mono rpm"
            type="text"
            inputmode="decimal"
            :placeholder="String(Math.round(status?.spindle.commanded_rpm ?? 0))"
            autocomplete="off"
            aria-label="Spindle speed, rpm"
          />
          <span class="hint">rpm</span>
          <button class="btn" type="button" :class="{ on: status?.spindle.direction === 'reverse' }" :disabled="spindleBlock !== null" :title="spindleBlock ?? 'Spindle reverse (M4)'" @click="spindle('reverse')">
            <AppIcon name="reverse" />Reverse
          </button>
          <button class="btn" type="button" :disabled="manualBlock !== null" :title="manualBlock ?? 'Spindle stop (M5)'" @click="send({ type: 'set_spindle', direction: 'stopped', rpm: 0 })">
            <AppIcon name="stop" />Stop
          </button>
          <button class="btn" type="button" :class="{ on: status?.spindle.direction === 'forward' }" :disabled="spindleBlock !== null" :title="spindleBlock ?? 'Spindle forward (M3)'" @click="spindle('forward')">
            <AppIcon name="forward" />Forward
          </button>
        </div>
        <div class="line">
          <span class="label">Coolant</span>
          <button class="btn" type="button" :class="{ on: status?.coolant.flood }" :disabled="coolantBlock !== null" :title="coolantBlock ?? 'Flood coolant (M8)'" @click="coolant('flood')">
            <AppIcon name="drop" />Flood
          </button>
          <button class="btn" type="button" :class="{ on: status?.coolant.mist }" :disabled="coolantBlock !== null" :title="coolantBlock ?? 'Mist coolant (M7)'" @click="coolant('mist')">
            Mist
          </button>
        </div>
      </section>

      <section class="card">
        <h2>Work coordinate system</h2>
        <div class="segmented wcs">
          <button
            v-for="(name, index) in wcsNames"
            :key="name"
            class="btn mono"
            type="button"
            :class="{ on: status?.offsets.wcs === index + 1 }"
            :disabled="mdiBlock !== null"
            :title="mdiBlock ?? `Make ${name} the active work system. The machine does not move.`"
            @click="send({ type: 'select_wcs', wcs: index + 1 })"
          >
            {{ name }}
          </button>
        </div>
      </section>
    </div>
  </div>
</template>

<style scoped>
.manual {
  display: grid;
  grid-template-columns: 404px minmax(0, 1fr);
  gap: 10px;
  height: 100%;
  min-height: 0;
}

.side {
  display: flex;
  flex-direction: column;
  gap: 10px;
  min-width: 0;
  min-height: 0;
}

.jog {
  gap: 10px;
}

.pads {
  display: flex;
  justify-content: center;
}

.pad {
  display: grid;
  grid-template-areas:
    '. ypos . gap zpos'
    'xneg stop xpos gap .'
    '. yneg . gap zneg';
  grid-template-columns: 88px 88px 88px 16px 88px;
  grid-template-rows: 76px 76px 76px;
  gap: 6px;
}

.jogbtn {
  flex-direction: column;
  gap: 0;
  padding: 0;
  font-size: 15px;
  touch-action: none;
}

.stopjog {
  grid-area: stop;
  font-size: 13px;
}

.line {
  display: flex;
  align-items: center;
  gap: 8px;
}

.line > .label {
  width: 44px;
  flex: none;
}

.line input[type='range'] {
  flex: 1;
}

.jog .segmented .btn {
  padding: 0 10px;
}

.speed {
  width: 44px;
  text-align: right;
  font-weight: 600;
}

.homeaxis {
  flex: 1;
  flex-direction: column;
  gap: 0;
  padding: 0 6px;
}

.homestate {
  font-size: 10px;
  font-weight: 600;
  letter-spacing: 0.04em;
  text-transform: uppercase;
  color: var(--warn);
}

.homestate.homed {
  color: var(--go);
}

.keys {
  margin: 0;
}

.blocked {
  margin: 0;
}

.mdi {
  flex: 1;
}

.history {
  display: flex;
  flex: 1;
  flex-direction: column;
  gap: 2px;
  min-height: 44px;
  padding: 4px;
  border: 1px solid var(--line);
  border-radius: var(--radius);
  background: var(--bg-sunken);
  overflow-y: auto;
}

.history .hint {
  margin: 8px;
}

.entry {
  display: flex;
  align-items: baseline;
  justify-content: space-between;
  gap: 12px;
  min-height: 30px;
  padding: 4px 8px;
  border: 0;
  border-radius: 4px;
  background: none;
  text-align: left;
  cursor: pointer;
}

.entry:hover {
  background: var(--control);
}

.entry .result {
  font-size: 12px;
  color: var(--text-dim);
  text-align: right;
}

.entry.rejected .result,
.entry.failed .result,
.entry.timed_out .result {
  color: var(--stop);
}

.entry.rejected .text,
.entry.failed .text,
.entry.timed_out .text {
  color: var(--text-dim);
}

.grow {
  flex: 1;
  min-width: 0;
}

.side .line > .label {
  width: 58px;
}

.side .line .btn {
  padding: 0 12px;
}

.rpm {
  width: 78px;
  text-align: right;
}

.spacer {
  flex: 1;
}

.wcs {
  display: flex;
}

.wcs .btn {
  flex: 1;
  padding: 0;
  font-size: 13px;
}
</style>
