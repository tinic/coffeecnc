<script lang="ts">
/** The motion mode of the jog card, kept while CoffeeCNC runs. */
const manualMotion: { mode: 'jog' | 'feed' | 'positions' } = { mode: 'jog' }
</script>

<script setup lang="ts">
import WorkSystemSelector from './WorkSystemSelector.vue'
import { vTooltip } from '../tooltip'
// Manual workspace: homing, jogging, MDI, spindle, coolant and work system selection.
//
// Jogging follows jogHold.ts: a jog runs only while a finger, mouse button or key is known to be
// down, and the keyboard jogs only while this workspace is shown, no dialog is open and no field
// has the focus.
import { computed, nextTick, onBeforeUnmount, reactive, ref, useTemplateRef, watch } from 'vue'
import { useI18n } from 'vue-i18n'

import { useCardFeedback } from '../card-feedback'
import { homingMessage, powerBlock } from '../machine-block'
import { openDialogs } from '../dialogs'
import { translateMessage } from '../i18n/messages'
import { vNumeric } from '../numeric-input'
import { parseNumericExpression } from '../numeric-expression'
import { JogHold, Keys, Touches, capturedPress } from '../jogHold'
import { axisJogVelocity, displayJogIncrements } from '../machine-capabilities'
import { allHomed, axes, homingActive, live, manualBlock, mdiBlock, onResult, panel, send } from '../machine'
import type { AxisLetter, AxisStatus } from '../protocol'
import { touchProbeLoaded } from '../tool-models'
import { formatIncrement, formatLength, formatVelocity, velocityUnit, wcsName } from '../units'
import { workOffsetRows } from '../work-offsets'
import AppIcon from './AppIcon.vue'
import MaterialProbeControls from './MaterialProbeControls.vue'
import SetterStatusBand from './SetterStatusBand.vue'
import MachineFeedback from './MachineFeedback.vue'
import MdiMacros from './MdiMacros.vue'
import MotionPad from './MotionPad.vue'
import PowerFeedControls from './PowerFeedControls.vue'
import SavedPositions from './SavedPositions.vue'
import type { SetterStatusLine } from '../setter-state'

type JogKey = 'x+' | 'x-' | 'y+' | 'y-' | 'z+' | 'z-'
const props = defineProps<{ touchOff?: boolean }>()
const { t } = useI18n()

/** Machine and backend reasons arrive in English; the panel's own text is already translated. */
function reason(text: string | null): string | null {
  return text === null ? null : translateMessage(text)
}
function shownLine(line: SetterStatusLine): SetterStatusLine {
  return { ...line, text: translateMessage(line.text) }
}

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
const coolantOutputs = computed(() => panel.hello?.coolant ?? { flood: true, mist: true })
const activeOffset = computed(() => workOffsetRows(panel.workOffsets ?? null, status.value, panel.link)
  .find((system) => system.wcs === status.value?.offsets.wcs))
const held = reactive(new Set<JogKey>())
const increments = computed(() => displayJogIncrements(panel.hello, unit.value))
const stepUnavailable = computed(() => panel.jogIncrement !== null && !increments.value.includes(panel.jogIncrement) ? unit.value === 'inch' ? t('manual.tab.chooseStepInches') : t('manual.tab.chooseStepMillimeters') : null)
const jogMachineBlock = computed(() => manualBlock.value ?? (homingActive.value ? homingMessage() : null))
const jogUnavailable = computed(() => jogMachineBlock.value ?? stepUnavailable.value)
const materialProbeTool = computed(() => panel.hello?.fixed_setter?.material_travel_mm
  ? panel.hello.touch_probe_tool : undefined)

const motionModes = [
  ['jog', 'manual.tab.modeJog', 'manual.tab.modeJogTip'],
  ['feed', 'manual.tab.modeFeed', 'manual.tab.modeFeedTip'],
  ['positions', 'manual.tab.modePositions', 'manual.tab.modePositionsTip'],
] as const
type MotionMode = (typeof motionModes)[number][0]
const motionMode = ref<MotionMode>(manualMotion.mode)
const mode = computed<MotionMode>(() => props.touchOff ? 'jog' : motionMode.value)
const modeLine = ref<SetterStatusLine>({ text: '', level: 'info' })
function chooseMode(next: MotionMode): void {
  motionMode.value = next
  manualMotion.mode = next
  modeLine.value = { text: '', level: 'info' }
}

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
  if (!allowJog(jogBlock(key))) return
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

/** A field has the keyboard. A slider does not: the arrow and page keys keep jogging. */
function typing(): boolean {
  const element = document.activeElement
  return (
    (element instanceof HTMLInputElement && element.type !== 'range') ||
    element instanceof HTMLTextAreaElement ||
    element instanceof HTMLSelectElement ||
    (element instanceof HTMLElement && element.isContentEditable)
  )
}

function keyDown(event: KeyboardEvent): void {
  const key = keyboard[event.code]
  if (key === undefined || mode.value !== 'jog' || typing() || openDialogs.value > 0 || event.ctrlKey || event.altKey || event.metaKey) {
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
watch(mode, lostAttention, { flush: 'sync' })

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

function jogBlock(key: JogKey): string | null {
  if (jogUnavailable.value !== null) return jogUnavailable.value
  return axisJogVelocity(panel.hello, jogs[key].axis, panel.jogVelocity) > 0 ? null : t('manual.tab.jogSpeedUnavailable')
}
const limitedSpeeds = computed(() => (panel.hello?.axes ?? [])
  .map((axis) => ({ axis, speed: axisJogVelocity(panel.hello, axis, panel.jogVelocity) }))
  .filter(({ speed }) => speed > 0 && speed < panel.jogVelocity)
  .map(({ axis, speed }) => `${axis.toUpperCase()} ${formatVelocity(speed, unit.value)} ${velocityUnit(unit.value)}`).join(' · '))

const { line: jogLine, allow: allowJog } = useCardFeedback(
  () => limitedSpeeds.value ? t('manual.tab.axisLimits', { limits: limitedSpeeds.value }) : '',
  () => stepUnavailable.value,
  [jogMachineBlock, stepUnavailable, (): number => panel.jogVelocity, (): unknown => panel.hello],
)

function showJogBlock(key: JogKey): void {
  const reason = jogBlock(key)
  if (reason !== null) allowJog(reason)
}

function jogLabel(key: JogKey): string {
  return `${key.charAt(0).toUpperCase()}${key.charAt(1) === '+' ? '+' : '−'}`
}

// ---- homing ---------------------------------------------------------------------------------------

// On a machine that homes on reference marks, homing finds a mark only after the axis has
// crossed it, by a jog or by hand with the machine off.
function markBlock(axis: AxisStatus): string | null {
  return axis.reference_mark === false && !axis.homing
    ? t('manual.tab.crossMarkFirst', { axis: axis.axis.toUpperCase() })
    : null
}
const marksBlock = computed(() => axes.value.map(markBlock).find((reason) => reason !== null) ?? null)

function homeState(axis: AxisStatus): string {
  if (axis.homing) return t('manual.tab.stateHoming')
  if (axis.homed) return t('manual.tab.stateHomed')
  if (axis.at_reference_mark === true) return t('manual.tab.stateAtMark')
  return axis.reference_mark === false ? t('manual.tab.stateFindMark') : t('manual.tab.stateNotHomed')
}

function home(axis: AxisLetter | 'all'): void {
  const status = axes.value.find((candidate) => candidate.axis === axis)
  const mark = axis === 'all' ? marksBlock.value : status === undefined ? null : markBlock(status)
  if (allowJog(jogMachineBlock.value ?? mark)) send({ type: 'home', axis })
}

// ---- MDI ------------------------------------------------------------------------------------------

const mdiText = ref<string>('')
const mdiList = useTemplateRef<HTMLElement>('mdiList')
let recall = -1
const mdiLocalBlock = computed(() => mdiBlock.value !== jogMachineBlock.value ? mdiBlock.value : null)
const { line: mdiLine, allow: allowMdi } = useCardFeedback(
  () => '',
  () => mdiLocalBlock.value,
  [mdiBlock, (): boolean => mdiText.value.trim() === ''],
)

function sendMdi(): void {
  const text = mdiText.value.trim()
  if (!allowMdi(mdiBlock.value ?? (text === '' ? t('manual.tab.enterMdi') : null))) return
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
const reportedRpm = computed(() => {
  const spindle = status.value?.spindle
  return spindle && (spindle.commanded_rpm > 0 || spindle.direction !== 'stopped')
    ? spindle.commanded_rpm : panel.startupSpindleRpm
})
const rpmText = computed({
  get: () => rpmDraft.value ?? String(presetRpm.value ?? reportedRpm.value),
  set: (value: string) => { rpmDraft.value = value },
})
const rpm = computed(() => parseNumericExpression(rpmText.value, 'rpm'))
const spindleLocalBlock = computed((): string | null => {
  if (touchProbeLoaded.value) return t('manual.moves.probeLoaded')
  return rpm.value === null || !Number.isFinite(rpm.value) || rpm.value <= 0 ? t('manual.tab.enterSpindleSpeed') : null
})
const spindleBlock = computed(() => manualBlock.value ?? spindleLocalBlock.value)
const coolantBlock = computed(() => powerBlock(panel))
const { line: spindleLine, allow: allowSpindle } = useCardFeedback(
  () => '',
  () => touchProbeLoaded.value ? spindleLocalBlock.value : null,
  [manualBlock, coolantBlock, spindleLocalBlock],
)

function spindle(direction: 'forward' | 'reverse'): void {
  if (allowSpindle(spindleBlock.value) && rpm.value !== null) {
    send({ type: 'set_spindle', direction, rpm: rpm.value })
    presetRpm.value = null
  }
}

function spindleStop(): void {
  if (allowSpindle(manualBlock.value)) send({ type: 'set_spindle', direction: 'stopped', rpm: 0 })
}

function confirmSpindleSpeed(event: Event): void {
  if (event instanceof KeyboardEvent && event.repeat) return
  if (rpmDraft.value === null || manualBlock.value !== null || rpm.value === null || rpm.value <= 0) return
  const direction = status.value?.spindle.direction
  if (direction === 'forward' || direction === 'reverse') {
    if (touchProbeLoaded.value) return
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

function coolant(which: 'flood' | 'mist'): void {
  if (!coolantOutputs.value[which] || !allowSpindle(coolantBlock.value)) return
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
      <div class="jog-heading">
        <h2 v-if="touchOff">{{ t('manual.tab.jog') }}</h2>
        <div v-else class="segmented modes" role="tablist" :aria-label="t('manual.tab.manualMotion')">
          <button
            v-for="[value, label, tip] in motionModes"
            :key="value"
            v-tooltip="t(tip)"
            class="btn"
            type="button"
            role="tab"
            :aria-selected="mode === value"
            :class="{ on: mode === value }"
            @click="chooseMode(value)"
          >
            {{ t(label) }}
          </button>
        </div>
      </div>
      <PowerFeedControls v-if="mode === 'feed'" @line="modeLine = $event" />
      <SavedPositions v-else-if="mode === 'positions'" @line="modeLine = $event" />
      <MotionPad v-if="mode === 'jog'" class="jog-controls">
        <template #direction="{ jog, area, icon }">
          <button
            v-tooltip="reason(jogBlock(jog)) ?? t('manual.tab.jogDirection', { direction: jogLabel(jog) })"
            class="btn jogbtn"
            type="button"
            :class="{ held: held.has(jog) }"
            :style="{ gridArea: area }"
            :aria-disabled="jogBlock(jog) !== null"
            @click="showJogBlock(jog)"
            @touchstart.prevent="jogTouchStart(jog, $event)"
            @touchend.prevent="jogTouchEnd(jog, $event)"
            @touchcancel="jogTouchEnd(jog, $event)"
            @pointerdown="jogPointerDown(jog, $event)"
            @pointerup="jogHold.release(jog)"
            @pointercancel="jogHold.release(jog)"
            @lostpointercapture="jogHold.release(jog)"
            @contextmenu.prevent
          >
            <AppIcon :name="icon" />
            <span class="mono">{{ jogLabel(jog) }}</span>
          </button>
        </template>
        <template #stop>
          <button
            v-tooltip="t('manual.tab.stopAllJogging')"
            class="btn danger jogbtn stopjog"
            type="button"
            @touchstart.prevent="stopTouch"
            @pointerdown="stopPointerDown"
          >
            {{ t('manual.tab.stopJog') }}
          </button>
        </template>
        <div class="jog-parameters">
          <div class="steps">
            <div class="step-heading"><span class="label">{{ t('manual.tab.step') }}</span><span class="hint">{{ unit === 'inch' ? 'in' : 'mm' }}</span></div>
            <div class="increments">
              <button
                v-tooltip="t('manual.tab.continuousTip')"
                class="btn"
                type="button"
                :class="{ on: panel.jogIncrement === null }"
                @click="panel.jogIncrement = null"
              >
                {{ t('manual.tab.continuous') }}
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
          <label class="jog-speed">
            <span class="step-heading">
              <span class="label">{{ t('manual.tab.speed') }}</span>
              <span><strong class="speed mono">{{ formatIncrement(panel.jogVelocity, unit) }}</strong> <span class="hint">{{ velocityUnit(unit) }}</span></span>
            </span>
            <input
              type="range"
              min="10"
              :max="panel.hello?.max_jog_velocity ?? 10"
              step="10"
              :value="panel.jogVelocity"
              :aria-label="t('manual.tab.jogSpeed')"
              :aria-valuetext="`${formatIncrement(panel.jogVelocity, unit)} ${velocityUnit(unit)}`"
              @input="setJogVelocity"
            />
          </label>
        </div>
      </MotionPad>

      <div v-if="!touchOff" class="line homes">
        <span class="label">{{ t('manual.tab.home') }}</span>
        <button
          v-tooltip="reason(jogMachineBlock) ?? marksBlock ?? t('manual.tab.homeAllTip')"
          class="btn"
          type="button"
          :aria-disabled="jogMachineBlock !== null || marksBlock !== null"
          @click="home('all')"
        >
          <AppIcon name="home" />{{ t('manual.tab.homeAll') }}
        </button>
        <button
          v-for="axis in axes"
          :key="axis.axis"
          v-tooltip="reason(jogMachineBlock) ?? markBlock(axis) ?? t('manual.tab.homeAxis', { axis: axis.axis.toUpperCase() })"
          class="btn homeaxis"
          :class="{ marked: axis.at_reference_mark === true && !axis.homed }"
          type="button"
          :aria-disabled="jogMachineBlock !== null || markBlock(axis) !== null"
          @click="home(axis.axis)"
        >
          <span class="homeletter">{{ axis.axis.toUpperCase() }}</span>
          <span class="homestate" :class="{ homed: axis.homed }">{{ homeState(axis) }}</span>
        </button>
      </div>
      <SetterStatusBand class="jog-status" :line="shownLine(mode === 'jog' ? jogLine : modeLine)" />
    </section>

    <div class="side" :class="{ 'probe-side': touchOff }">
      <section v-if="touchOff" class="card probe-card">
        <div class="wcs-heading">
          <h2>{{ materialProbeTool ? t('manual.tab.probeTool', { tool: materialProbeTool }) : t('manual.tab.probe') }}</h2>
          <span v-if="materialProbeTool && live" class="hint" :class="{ 'probe-contact': status?.probe_input }">
            {{ status?.probe_input === true ? t('manual.tab.probeContact') : status?.probe_input === false ? t('manual.tab.probeClear') : t('manual.tab.probeUnavailable') }}
          </span>
        </div>
        <MaterialProbeControls v-if="materialProbeTool" :tool="materialProbeTool" />
        <p v-else class="hint">{{ t('manual.tab.materialProbeUnavailable') }}</p>
      </section>
      <section v-else class="card mdi">
        <h2>MDI</h2>
        <div ref="mdiList" class="history">
          <button
            v-for="entry in panel.mdi"
            :key="entry.id"
            v-tooltip="t('manual.tab.copyToEntry')"
            class="entry"
            type="button"
            :class="entry.outcome"
            @click="mdiText = entry.text"
          >
            <span class="mono text">{{ entry.text }}</span>
            <span class="result">
              <template v-if="entry.outcome === 'pending'">{{ t('manual.tab.mdiSent') }}</template>
              <template v-else-if="entry.outcome === 'accepted'">{{ t('manual.tab.mdiAccepted') }}</template>
              <template v-else>{{ translateMessage(entry.reason) }}</template>
            </span>
          </button>
        </div>
        <form class="line" @submit.prevent="sendMdi">
          <input
            v-model="mdiText"
            class="field mono grow"
            type="text"
            :placeholder="t('manual.tab.mdiPlaceholder')"
            autocomplete="off"
            autocapitalize="characters"
            spellcheck="false"
            :aria-label="t('manual.tab.mdiCommand')"
            @keydown.up.prevent="recallMdi(-1)"
            @keydown.down.prevent="recallMdi(1)"
          />
          <MdiMacros />
          <button v-tooltip="reason(mdiBlock) ?? t('manual.tab.runCommandTip')" class="btn primary" type="submit" :aria-disabled="mdiBlock !== null || mdiText.trim() === ''">
            {{ t('manual.tab.run') }}
          </button>
        </form>
        <SetterStatusBand :line="shownLine(mdiLine)" />
      </section>

      <section v-if="!touchOff" class="card utilities">
        <h2>{{ coolantOutputs.flood || coolantOutputs.mist ? t('manual.tab.spindleAndCoolant') : t('manual.tab.spindle') }}</h2>
        <div class="line">
          <input
            v-model="rpmText"
            v-numeric
            class="field mono rpm"
            type="text"
            inputmode="decimal"
            :disabled="manualBlock !== null"
            autocomplete="off"
            :aria-label="t('manual.tab.spindleSpeed')"
            :data-numeric-effect="t('manual.tab.spindleSpeedEffect')"
            :data-numeric-action="t('manual.tab.setRpm')"
            data-numeric-unit="rpm"
            @numeric-confirm="confirmSpindleSpeed"
            @keydown.enter.prevent="confirmSpindleSpeed"
            @keydown.esc.prevent="discardSpindleEdit"
            @blur="discardSpindleEdit"
          />
          <span class="hint">rpm</span>
          <button v-tooltip="reason(spindleBlock) ?? t('manual.tab.spindleReverseTip')" class="btn" type="button" :class="{ on: status?.spindle.direction === 'reverse' }" :aria-disabled="spindleBlock !== null" @click="spindle('reverse')">
            <AppIcon name="reverse" />{{ t('manual.tab.reverse') }}
          </button>
          <button v-tooltip="reason(manualBlock) ?? t('manual.tab.spindleStopTip')" class="btn" type="button" :aria-disabled="manualBlock !== null" @click="spindleStop">
            <AppIcon name="stop" />{{ t('manual.tab.stop') }}
          </button>
          <button v-tooltip="reason(spindleBlock) ?? t('manual.tab.spindleForwardTip')" class="btn" type="button" :class="{ on: status?.spindle.direction === 'forward' }" :aria-disabled="spindleBlock !== null" @click="spindle('forward')">
            <AppIcon name="forward" />{{ t('manual.tab.forward') }}
          </button>
        </div>
        <div v-if="coolantOutputs.flood || coolantOutputs.mist" class="line coolant">
          <span class="label">{{ t('manual.tab.coolant') }}</span>
          <button v-if="coolantOutputs.flood" v-tooltip="reason(coolantBlock) ?? t('manual.tab.floodTip')" class="btn" type="button" :class="{ on: status?.coolant.flood }" :aria-disabled="coolantBlock !== null" @click="coolant('flood')">
            <AppIcon name="drop" />{{ t('manual.tab.flood') }}
          </button>
          <button v-if="coolantOutputs.mist" v-tooltip="reason(coolantBlock) ?? t('manual.tab.mistTip')" class="btn" type="button" :class="{ on: status?.coolant.mist }" :aria-disabled="coolantBlock !== null" @click="coolant('mist')">
            {{ t('manual.tab.mist') }}
          </button>
        </div>
        <MachineFeedback show-probe class="sensor-feedback" />
        <SetterStatusBand class="spindle-status" :line="shownLine(spindleLine)" />
      </section>

      <section class="card work-system">
        <div class="wcs-heading">
          <h2>{{ t('manual.tab.workSystem') }}</h2>
          <div class="wcs-values" :aria-label="t('manual.tab.activeOffsets')">
            <span>{{ activeOffset?.stale ? t('manual.tab.lastReportedOf', { wcs: status ? wcsName(status.offsets.wcs) : '—' }) : t('manual.tab.offsetOf', { wcs: status ? wcsName(status.offsets.wcs) : '—' }) }}</span>
            <span v-for="(axis, index) in ['X', 'Y', 'Z']" :key="axis">{{ axis }} <strong class="mono">{{ activeOffset?.offset ? formatLength(activeOffset.offset[index]!, unit) : '—' }}</strong></span>
            <span>{{ unit === 'inch' ? 'in' : 'mm' }}</span>
            <span v-if="activeOffset?.rotation_xy">R {{ activeOffset.rotation_xy }}°</span>
          </div>
        </div>
        <WorkSystemSelector />
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
  gap: 8px;
  min-width: 0;
  min-height: 0;
  overflow-y: auto;
}

.side > .card {
  flex-shrink: 0;
  padding: 9px 12px;
  gap: 6px;
}

.jog {
  --motion-content-height: 236px;
  gap: 8px;
  align-self: start;
  max-height: 100%;
  overflow-y: auto;
}
.jogbtn { touch-action: none; }
.stopjog { white-space: normal; line-height: 1.15; text-align: center; overflow-wrap: anywhere; hyphens: auto; padding-inline: 4px; }
.jog-parameters, .steps, .jog-speed { display: flex; flex-direction: column; gap: 4px; min-width: 0; }
.jog-parameters { gap: 6px; }
.jog-speed input { width: 100%; min-height: 44px; margin: 0; }

.line {
  display: flex;
  align-items: center;
  gap: 8px;
}

.line > .label {
  min-width: 44px;
  flex: none;
}

.step-heading { display: flex; justify-content: space-between; align-items: baseline; }
.increments { display: grid; grid-template-columns: repeat(4, minmax(44px, 1fr)); gap: 4px; }
.increments > :first-child { grid-column: 1 / -1; }
.increments .btn { padding-inline: 3px; font-size: 12px; }
.speed { font-weight: 600; }
.homes .btn { flex: 1; min-height: 48px; }
.jog-status { margin-block: 0; }

.homeaxis {
  flex: 1;
  flex-direction: column;
  gap: 0;
  padding: 0 6px;
}

.homeletter {
  padding: 0 8px;
  border-radius: 3px;
}

/* The axis stands at its reference mark, also with the machine off. Fixed colours, so that the
   letter reads as red on the light and the dark panel, also while the button is dimmed. */
.homeaxis.marked .homeletter {
  background: #d92d20;
  color: #ffffff;
}

.homestate {
  max-width: 100%;
  white-space: normal;
  line-height: 1.1;
  text-align: center;
  overflow-wrap: anywhere;
  font-size: 10px;
  font-weight: 600;
  letter-spacing: 0.04em;
  text-transform: uppercase;
  color: var(--warn);
}

.homestate.homed {
  color: var(--go);
}

.btn[aria-disabled="true"] { opacity: .68; }

.side > .mdi {
  display: grid;
  grid-template-rows: auto minmax(34px, 1fr) auto auto;
  flex: 1 1 0;
  min-height: min-content;
}
.mdi > .setter-band, .utilities > .setter-band { margin-block: 0; }
.work-system { margin-top: auto; }
.side > .probe-card { gap: 12px; padding: 14px; }
.probe-contact { color: var(--warn); }
.modes { display: flex; }
.modes .btn { flex: 1; min-width: 44px; min-height: 44px; padding: 0 12px; font-size: 13px; }
.jog-heading h2, .wcs-heading h2 { margin: 0; font-size: 11px; font-weight: 600; letter-spacing: .07em; text-transform: uppercase; color: var(--text-dim); }
.wcs-heading { display: flex; align-items: center; justify-content: space-between; gap: 12px; }.wcs-heading h2 { flex: none; }

.history {
  display: flex;
  flex-direction: column;
  gap: 2px;
  min-height: 0;
  /* A long history scrolls inside the card instead of pushing the cards below off the screen. */
  contain: size;
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
.wcs-values { display: flex; flex-wrap: wrap; align-items: center; gap: 4px 8px; min-height: 24px; font-size: 11px; color: var(--text-dim); }.wcs-values > span { white-space: nowrap; }.wcs-values strong { color: var(--text); font-weight: 400; }
.utilities { display: flex; flex-flow: row wrap; align-content: start; column-gap: 14px; }
.utilities > :not(.line) { flex: 1 0 100%; }
.utilities > .line { flex: 1 0 auto; }
.utilities > .line.coolant { flex: 0 0 auto; }
.utilities .line .btn { white-space: nowrap; }
.utilities .sensor-feedback { margin-top: 0; }
.utilities .line { gap: 6px; }
.utilities .line .btn { padding: 0 10px; font-size: 12px; }
.utilities .line > .label { width: auto; }
@media (max-height: 760px) {
  .jog { gap: 4px; padding-block: 8px; }
  .homes { gap: 4px; }
  .homes .btn { min-height: 44px; }
  .side { gap: 6px; }.side > .card { gap: 4px; padding-block: 8px; }.history .hint { margin: 2px 8px; }.wcs-values { min-height: 18px; }
}
</style>
