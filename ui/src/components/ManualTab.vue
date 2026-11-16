<script setup lang="ts">
import { vTooltip } from '../tooltip'
// Manual workspace: homing, jogging, MDI, spindle, coolant and work system selection.
//
// Jogging follows jogHold.ts: a jog runs only while a finger, mouse button or key is known to be
// down, and the keyboard jogs only while this workspace is shown, no dialog is open and no field
// has the focus.
import { computed, nextTick, onBeforeUnmount, reactive, ref, useTemplateRef, watch } from 'vue'

import { openDialogs } from '../dialogs'
import { vNumeric } from '../numeric-input'
import { parseNumericExpression } from '../numeric-expression'
import { JogHold, Keys, Touches, capturedPress } from '../jogHold'
import { axisJogVelocity, displayJogIncrements } from '../machine-capabilities'
import { allHomed, axes, homingActive, manualBlock, mdiBlock, onResult, panel, send } from '../machine'
import type { AxisLetter } from '../protocol'
import { formatIncrement, formatVelocity, velocityUnit, wcsNames } from '../units'
import AppIcon from './AppIcon.vue'
import MachineFeedback from './MachineFeedback.vue'

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
const increments = computed(() => displayJogIncrements(panel.hello, unit.value))
const stepUnavailable = computed(() => panel.jogIncrement !== null && !increments.value.includes(panel.jogIncrement) ? `Choose a step in ${unit.value === 'inch' ? 'inches' : 'millimeters'}` : null)
const jogUnavailable = computed(() => manualBlock.value ?? (homingActive.value ? 'Homing is in progress' : stepUnavailable.value))

const jogHold = new JogHold<JogKey>(
  {
    send: (key: JogKey, isHeld: boolean): void => {
      if (isHeld) {
        held.add(key)
      } else {
        held.delete(key)
      }
      send({ type: 'jog', ...jogs[key], velocity: axisJogVelocity(panel.hello, jogs[key].axis, panel.jogVelocity), held: isHeld })
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
  if (jogUnavailable.value !== null || axisJogVelocity(panel.hello, jogs[key].axis, panel.jogVelocity) === 0) {
    return
  }
  const increment = panel.jogIncrement
  if (increment === null) {
    jogHold.press(key, () => jogUnavailable.value === null && stillHeld())
  } else {
    const jog = jogs[key]
    send({ type: 'jog_step', axis: jog.axis, distance: jog.direction * increment, velocity: axisJogVelocity(panel.hello, jog.axis, panel.jogVelocity) })
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
watch(() => panel.hello, lostAttention)
// Interrupt the physical gesture, not just the next request. Recovery requires a new press;
// an old held key or pointer must not restart after stale status, power loss or homing.
watch(jogUnavailable, (reason) => {
  if (reason !== null) lostAttention()
}, { flush: 'sync' })
watch(allHomed, lostAttention, { flush: 'sync' })
watch(unit, lostAttention, { flush: 'sync' })

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

function jogBlock(key: JogKey): string | null {
  if (jogUnavailable.value !== null) return jogUnavailable.value
  return axisJogVelocity(panel.hello, jogs[key].axis, panel.jogVelocity) > 0 ? null : 'Configured jog speed is unavailable'
}
const limitedSpeeds = computed(() => (panel.hello?.axes ?? [])
  .map((axis) => ({ axis, speed: axisJogVelocity(panel.hello, axis, panel.jogVelocity) }))
  .filter(({ speed }) => speed > 0 && speed < panel.jogVelocity)
  .map(({ axis, speed }) => `${axis.toUpperCase()} ${formatVelocity(speed, unit.value)} ${velocityUnit(unit.value)}`).join(' · '))

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

const rpmDraft = ref<string | null>(null)
const presetRpm = ref<number | null>(null)
const rpmText = computed({
  get: () => rpmDraft.value ?? String(presetRpm.value ?? status.value?.spindle.commanded_rpm ?? 0),
  set: (value: string) => { rpmDraft.value = value },
})
const rpm = computed(() => parseNumericExpression(rpmText.value, 'rpm'))
const spindleBlock = computed((): string | null => {
  if (manualBlock.value !== null) {
    return manualBlock.value
  }
  return rpm.value === null || !Number.isFinite(rpm.value) || rpm.value <= 0 ? 'Enter a spindle speed above zero' : null
})

function spindle(direction: 'forward' | 'reverse'): void {
  if (spindleBlock.value === null && rpm.value !== null) {
    send({ type: 'set_spindle', direction, rpm: rpm.value })
    presetRpm.value = null
  }
}

function confirmSpindleSpeed(event: Event): void {
  if (event instanceof KeyboardEvent && event.repeat) return
  if (rpmDraft.value === null || spindleBlock.value !== null || rpm.value === null) return
  const direction = status.value?.spindle.direction
  if (direction === 'forward' || direction === 'reverse') {
    send({ type: 'set_spindle', direction, rpm: rpm.value })
    presetRpm.value = null
  } else {
    // Choosing a speed while stopped never starts the spindle.
    presetRpm.value = rpm.value
  }
  rpmDraft.value = null
}

function discardSpindleEdit(): void {
  if (rpmDraft.value === null) return
  rpmDraft.value = null
  presetRpm.value = null
}

watch(() => status.value?.spindle.commanded_rpm, () => { presetRpm.value = null })
watch([(): unknown => panel.hello, manualBlock], () => {
  rpmDraft.value = null
  presetRpm.value = null
}, { flush: 'sync' })

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
      <p v-tooltip="jogUnavailable ?? undefined" class="blocked reserved"><template v-if="jogUnavailable">Jog unavailable: {{ jogUnavailable }}</template></p>
      <div class="jog-controls">
        <div class="pads">
          <div class="pad">
            <button
              v-for="[key, area, icon] in pad"
              :key="key"
              v-tooltip="jogBlock(key) ?? `Jog ${jogLabel(key)}`"
              class="btn jogbtn"
              type="button"
              :class="{ held: held.has(key) }"
              :style="{ gridArea: area }"
              :disabled="jogBlock(key) !== null"
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
              v-tooltip="'Stop all jogging'"
              class="btn danger jogbtn stopjog"
              type="button"
              @touchstart.prevent="stopTouch"
              @pointerdown="stopPointerDown"
            >
              Stop jog
            </button>
          </div>
        </div>

        <div class="steps">
          <div class="step-heading"><span class="label">Step</span><span class="hint">{{ unit === 'inch' ? 'in' : 'mm' }}</span></div>
          <div class="increments">
            <button
              v-tooltip="'Jog while the button or key is held'"
              class="btn"
              type="button"
              :class="{ on: panel.jogIncrement === null }"
              @click="panel.jogIncrement = null"
            >
              Continuous
            </button>
            <button
              v-for="increment in increments"
              :key="increment"
              class="btn mono"
              type="button"
              :class="{ on: panel.jogIncrement === increment }"
              @click="panel.jogIncrement = increment"
            >
              {{ formatIncrement(increment, unit) }}
            </button>
          </div>
        </div>
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

      <p v-tooltip="limitedSpeeds || undefined" class="hint reserved"><template v-if="limitedSpeeds">Axis limits: {{ limitedSpeeds }}</template></p>

      <div class="line">
        <span class="label">Home</span>
        <button
          v-tooltip="homeBlock ?? 'Home all axes using the configured sequence'"
          class="btn"
          type="button"
          :disabled="homeBlock !== null"
          @click="send({ type: 'home', axis: 'all' })"
        >
          <AppIcon name="home" />All
        </button>
        <button
          v-for="axis in axes"
          :key="axis.axis"
          v-tooltip="homeBlock ?? `Home ${axis.axis.toUpperCase()}`"
          class="btn homeaxis"
          type="button"
          :disabled="homeBlock !== null"
          @click="send({ type: 'home', axis: axis.axis })"
        >
          {{ axis.axis.toUpperCase() }}
          <span class="homestate" :class="{ homed: axis.homed }">{{
            axis.homing ? 'homing' : axis.homed ? 'homed' : 'not homed'
          }}</span>
        </button>
      </div>
      <p v-tooltip="'Keyboard jogging is active only in Manual with no input field focused.'" class="hint keys">
        Keyboard: arrows X/Y · Page Up/Down Z
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
            v-tooltip="'Copy to the entry field'"
            class="entry"
            type="button"
            :class="entry.outcome"
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
          <button v-tooltip="mdiBlock ?? 'Run this command'" class="btn primary" type="submit" :disabled="mdiBlock !== null || mdiText.trim() === ''">
            Run
          </button>
        </form>
        <p v-tooltip="mdiBlock ?? undefined" class="blocked reserved"><template v-if="mdiBlock">MDI unavailable: {{ mdiBlock }}</template></p>
      </section>

      <section class="card utilities">
        <h2>Spindle and coolant</h2>
        <div class="line">
          <input
            v-model="rpmText"
            v-numeric
            class="field mono rpm"
            type="text"
            inputmode="decimal"
            :disabled="manualBlock !== null"
            autocomplete="off"
            aria-label="Spindle speed, rpm"
            data-numeric-effect="Applies this RPM immediately if the spindle is running. When stopped, sets the speed for the next start."
            data-numeric-action="Set RPM"
            data-numeric-unit="rpm"
            @numeric-confirm="confirmSpindleSpeed"
            @keydown.enter.prevent="confirmSpindleSpeed"
            @keydown.esc.prevent="discardSpindleEdit"
            @blur="discardSpindleEdit"
          />
          <span class="hint">rpm</span>
          <button v-tooltip="spindleBlock ?? 'Spindle reverse (M4)'" class="btn" type="button" :class="{ on: status?.spindle.direction === 'reverse' }" :disabled="spindleBlock !== null" @click="spindle('reverse')">
            <AppIcon name="reverse" />Reverse
          </button>
          <button v-tooltip="manualBlock ?? 'Spindle stop (M5)'" class="btn" type="button" :disabled="manualBlock !== null" @click="send({ type: 'set_spindle', direction: 'stopped', rpm: 0 })">
            <AppIcon name="stop" />Stop
          </button>
          <button v-tooltip="spindleBlock ?? 'Spindle forward (M3)'" class="btn" type="button" :class="{ on: status?.spindle.direction === 'forward' }" :disabled="spindleBlock !== null" @click="spindle('forward')">
            <AppIcon name="forward" />Forward
          </button>
        </div>
        <div class="line">
          <span class="label">Coolant</span>
          <button v-tooltip="coolantBlock ?? 'Flood coolant (M8)'" class="btn" type="button" :class="{ on: status?.coolant.flood }" :disabled="coolantBlock !== null" @click="coolant('flood')">
            <AppIcon name="drop" />Flood
          </button>
          <button v-tooltip="coolantBlock ?? 'Mist coolant (M7)'" class="btn" type="button" :class="{ on: status?.coolant.mist }" :disabled="coolantBlock !== null" @click="coolant('mist')">
            Mist
          </button>
        </div>
        <MachineFeedback show-probe class="sensor-feedback" />
      </section>

      <section class="card work-system">
        <h2>Work coordinate system</h2>
        <div class="segmented wcs">
          <button
            v-for="(name, index) in wcsNames"
            :key="name"
            v-tooltip="mdiBlock ?? `Make ${name} the active work system. The machine does not move.`"
            class="btn mono"
            type="button"
            :class="{ on: status?.offsets.wcs === index + 1 }"
            :disabled="mdiBlock !== null"
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
  grid-template-columns: 570px minmax(0, 1fr);
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
  overflow-y: auto;
}

.side > .card {
  flex-shrink: 0;
  padding: 10px 12px;
  gap: 8px;
}

.jog {
  gap: 6px;
  align-self: start;
  max-height: 100%;
  overflow-y: auto;
}

.jog-controls { display: grid; grid-template-columns: 300px minmax(0, 1fr); gap: 12px; align-items: center; }
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
  grid-template-columns: 68px 68px 68px 12px 68px;
  grid-template-rows: 68px 68px 68px;
  gap: 4px;
}

.jogbtn {
  flex-direction: column;
  gap: 0;
  padding: 0;
  font-size: 20px;
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

.steps { display: flex; flex-direction: column; gap: 6px; min-width: 0; }
.step-heading { display: flex; justify-content: space-between; }
.increments { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 4px; }
.increments > :first-child { grid-column: 1 / -1; }
.increments .btn { min-width: 44px; padding: 0 10px; }

.speed {
  flex: none;
  width: 7ch;
  min-width: 7ch;
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
  font-size: 10px;
}

.blocked {
  margin: 0;
}
.reserved { height: 30px; flex: none; margin: 0; font-size: 11px; line-height: 15px; display: -webkit-box; -webkit-line-clamp: 2; -webkit-box-orient: vertical; overflow: hidden; }

.side > .mdi { flex: 1; min-height: 184px; }
.work-system { margin-top: auto; }

.history {
  display: flex;
  flex: 1;
  flex-direction: column;
  gap: 2px;
  min-height: 64px;
  height: 64px;
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
  flex: 1;
  min-width: 0;
  font-size: 12px;
  color: var(--text-dim);
  text-align: right;
}
.entry .text { flex: none; max-width: 45%; white-space: pre; overflow: hidden; text-overflow: ellipsis; }

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
  flex: none;
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
.utilities { display: grid; grid-template-columns: minmax(0, 1fr) auto; column-gap: 14px; }
.utilities h2 { grid-column: 1 / -1; }
.utilities .sensor-feedback { grid-column: 1 / -1; }
.utilities .line { gap: 6px; }
.utilities .line .btn { padding: 0 10px; font-size: 12px; }
.utilities .line > .label { width: auto; }
@media (max-width: 1150px) { .utilities { grid-template-columns: 1fr; } }
@media (max-height: 820px) { .jog { gap: 4px; }.reserved { height: 15px; -webkit-line-clamp: 1; } }
</style>
