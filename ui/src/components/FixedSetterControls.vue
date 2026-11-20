<script setup lang="ts">
import { computed, onBeforeUnmount, ref, watch } from 'vue'
import { allHomed, mdiBlock, onResult, panel, send } from '../machine'
import type { CommandBody, FixedSetterLimits, SetterState, Vec3 } from '../protocol'
import { formatLength, fromDisplay, toDisplay, wcsName } from '../units'
import ModalDialog from './ModalDialog.vue'
import NumberDialog from './NumberDialog.vue'

const props = defineProps<{ limits: FixedSetterLimits }>()
type Request = Extract<CommandBody, { type: 'fixed_setter' }>
type Move = Extract<Request, { action: 'calibrate' | 'measure' }>
type View = 'closed' | 'setup' | 'capture' | 'confirm' | 'working' | 'review'

const view = ref<View>('closed')
const state = ref<SetterState | null>(null)
const move = ref<Move | null>(null)
const pending = ref<{ id: number; action: Request['action'] } | null>(null)
const stopping = ref(false)
const error = ref('')
const status = computed(() => panel.status)
const unit = computed(() => panel.unit)
const suffix = computed(() => unit.value === 'inch' ? 'in' : 'mm')
const tool = computed(() => status.value?.tool.in_spindle ?? 0)
const wcs = computed(() => status.value?.offsets.wcs ?? 1)
const position = computed((): Vec3 | null => {
  const xyz = ['x', 'y', 'z'].map((letter) => status.value?.axes.find((axis) => axis.axis === letter)?.machine)
  const [x, y, z] = xyz
  return x !== undefined && y !== undefined && z !== undefined && [x, y, z].every(Number.isFinite)
    ? [x, y, z] : null
})
const block = computed(() => {
  if (mdiBlock.value !== null) return mdiBlock.value
  if (!allHomed.value || position.value === null) return 'Home XYZ first'
  const s = status.value
  if (s?.wait.kind === 'tool_change' || tool.value <= 0) return 'Finish loading a tool'
  const rpm = s?.spindle.actual_rpm
  if (rpm === null || rpm === undefined || rpm >= 1 || s?.spindle.direction !== 'stopped') {
    return 'Wait for the spindle to stop'
  }
  return s.motion.in_position ? null : 'Wait for the axes to stop'
})
const moveBlock = computed(() => status.value?.probe_input === false ? null : 'Check that the setter input is clear')
const atSetter = computed(() => {
  const plane = state.value?.calibration
  const xyz = position.value
  return !!plane && xyz !== null &&
    Math.hypot(xyz[0] - plane[0], xyz[1] - plane[1]) <= props.limits.xy_tolerance_mm
})
const moveChanged = computed(() => {
  const xyz = position.value
  const confirmed = move.value
  return xyz === null || tool.value !== confirmed?.tool ||
    [confirmed.x, confirmed.y, confirmed.z].some((v, i) => Math.abs(v - (xyz[i] ?? v)) > 0.02)
})
const defaultHeight = computed(() => String(Number(toDisplay(panel.preferences.tool_reference_height, unit.value).toFixed(6))))
const review = computed(() => state.value?.review ?? null)
const length = (value: number): string => `${formatLength(value, unit.value)} ${suffix.value}`

watch([tool, wcs, unit, (): unknown => panel.hello], (): void => {
  if (view.value === 'capture') close()
})

function request(command: Request): void {
  error.value = ''
  pending.value = { id: send(command), action: command.action }
}
function open(): void {
  state.value = null
  stopping.value = false
  view.value = 'setup'
  request({ type: 'fixed_setter', action: 'status' })
}
function capture(value: number, text: string): void {
  const xyz = position.value
  if (pending.value || xyz === null || block.value || tool.value !== props.limits.reference_tool) return
  view.value = 'setup'
  const height = text === defaultHeight.value ? panel.preferences.tool_reference_height : fromDisplay(value, unit.value)
  request({ type: 'fixed_setter', action: 'capture', tool: tool.value, wcs: wcs.value, height, z: xyz[2] })
}
function confirm(action: Move['action']): void {
  const xyz = position.value
  if (xyz === null || block.value || moveBlock.value || pending.value) return
  move.value = { type: 'fixed_setter', action, tool: tool.value, x: xyz[0], y: xyz[1], z: xyz[2] }
  view.value = 'confirm'
}
function start(): void {
  if (move.value === null || moveChanged.value || block.value || moveBlock.value) return
  view.value = 'working'
  stopping.value = false
  request(move.value)
}
function stop(): void {
  if (stopping.value) return
  stopping.value = true
  send({ type: 'abort' })
}
function close(): void {
  if (view.value === 'working') {
    stop()
    return
  }
  if (pending.value?.action === 'accept') return
  if (review.value) send({ type: 'fixed_setter', action: 'dismiss', review: review.value.id })
  view.value = 'closed'
  pending.value = null
}
function accept(): void {
  if (review.value && !pending.value && !block.value && tool.value === review.value.tool) {
    request({ type: 'fixed_setter', action: 'accept', review: review.value.id })
  }
}
const removeListener = onResult((command, result) => {
  if (command.type !== 'fixed_setter' || result.id !== pending.value?.id) return
  pending.value = null
  if (result.outcome !== 'accepted' || !result.setter) {
    if (view.value === 'working' && result.outcome === 'timed_out') stop()
    error.value = result.reason || 'No result from the fixed setter'
    state.value = null
    view.value = 'setup'
    return
  }
  state.value = result.setter
  if (stopping.value && review.value) {
    send({ type: 'fixed_setter', action: 'dismiss', review: review.value.id })
    state.value.review = null
  }
  stopping.value = false
  view.value = review.value ? 'review' : command.action === 'accept' ? 'closed' : 'setup'
})
onBeforeUnmount(() => {
  if (view.value === 'working') stop()
  if (review.value && pending.value?.action !== 'accept') {
    send({ type: 'fixed_setter', action: 'dismiss', review: review.value.id })
  }
  removeListener()
})
</script>

<template>
  <button class="btn" type="button" :disabled="panel.link !== 'live'" @click="open">Fixed setter</button>

  <ModalDialog v-if="view === 'setup'" title="Fixed tool setter" size="medium" expanded fresh-gesture @close="close">
    <p v-if="error" class="setter-error" role="alert">{{ error }}</p>
    <p v-if="pending?.action === 'status'" role="status">Reading setter state…</p>
    <p v-else-if="state?.calibration">
      Setter at machine X {{ length(state.calibration[0]) }} · Y {{ length(state.calibration[1]) }}.
      Jog above the setter within {{ length(limits.xy_tolerance_mm) }} of that center before measuring.
    </p>
    <p v-else>Calibrate with T{{ limits.reference_tool }}. Touch it to a known work Z surface or gauge block and capture the reference. Keep it clamped while jogging above the setter.</p>
    <p v-if="state?.reference_captured">Work reference captured. Jog above the setter before starting calibration.</p>
    <p v-if="block" role="status">{{ block }}</p>
    <p v-else-if="moveBlock" role="status">{{ moveBlock }}</p>
    <template #footer>
      <button v-if="state === null && !pending" class="btn" type="button" @click="open">Refresh</button>
      <button
        v-if="tool === limits.reference_tool" class="btn" type="button"
        :disabled="!!pending || block !== null" @click="view = 'capture'"
      >
        Capture work reference
      </button>
      <button
        v-if="state?.reference_captured" class="btn" type="button"
        :disabled="!!pending || block !== null || moveBlock !== null || tool !== limits.reference_tool"
        @click="confirm('calibrate')"
      >
        Calibrate setter
      </button>
      <button
        v-if="state?.calibration" class="btn primary" type="button"
        :disabled="!!pending || block !== null || moveBlock !== null || !atSetter"
        @click="confirm('measure')"
      >
        Measure length
      </button>
    </template>
  </ModalDialog>

  <NumberDialog
    v-if="view === 'capture'" :title="`Capture T${tool} work reference`"
    :effect="`With T${limits.reference_tool} touching the reference surface, enter its height in ${wcsName(wcs)}.`"
    label="Contact height" :unit="unit" :initial="defaultHeight" action="Capture reference"
    @confirm="capture" @close="view = 'setup'"
  />

  <ModalDialog
    v-if="view === 'confirm' && move" :title="move.action === 'calibrate' ? 'Calibrate setter?' : `Measure T${move.tool}?`"
    size="medium" expanded fresh-gesture @close="close"
  >
    <p>
      Confirm the tool is centered over the setter and the path is clear. Z will travel down up to
      {{ length(limits.travel_mm) }} at {{ length(limits.feed_mm_min) }}/min and return to the starting height after contact.
    </p>
    <p class="mono">Machine X {{ length(move.x) }} · Y {{ length(move.y) }} · Z {{ length(move.z) }}</p>
    <p v-if="block || moveBlock || moveChanged" role="status">{{ block ?? moveBlock ?? 'The tool or starting position changed' }}</p>
    <template #footer>
      <button class="btn" type="button" @click="view = 'setup'">Back</button>
      <button class="btn primary" type="button" :disabled="moveChanged || !!block || !!moveBlock" @click="start">Start {{ move.action === 'calibrate' ? 'calibration' : 'measurement' }}</button>
    </template>
  </ModalDialog>

  <ModalDialog v-if="view === 'working'" title="Fixed setter cycle" size="small" expanded @close="stop">
    <p role="status">{{ stopping ? 'Stopping the setter cycle…' : 'Searching for contact and returning to the starting height…' }}</p>
    <template #footer><button class="btn" type="button" :disabled="stopping" @click="stop">Stop</button></template>
  </ModalDialog>

  <ModalDialog
    v-if="view === 'review' && review" :title="review.calibration ? 'Review setter calibration' : `Review T${review.tool} length`"
    size="medium" expanded fresh-gesture @close="close"
  >
    <p v-if="review.calibration">
      Setter at machine X {{ length(review.contact[0]) }} · Y {{ length(review.contact[1]) }}.
      Trigger-plane Z {{ length(review.value) }}.
    </p>
    <dl v-if="!review.calibration || review.stopping_distance !== undefined" class="setter-values">
      <template v-if="!review.calibration">
        <dt>Current length</dt><dd class="mono">{{ length(review.previous_length) }}</dd>
        <dt>Measured length</dt><dd class="mono">{{ length(review.value) }}</dd>
        <dt>Difference</dt><dd class="mono">{{ length(review.value - review.previous_length) }}</dd>
      </template>
      <template v-if="review.stopping_distance !== undefined">
        <dt>Z travel after contact</dt><dd class="mono">{{ length(review.stopping_distance) }}</dd>
      </template>
    </dl>
    <p v-if="!review.calibration">Apply length saves the measured Z in T{{ review.tool }} and applies its compensation.</p>
    <p v-if="block" role="status">{{ block }}</p>
    <template #footer>
      <button class="btn" type="button" :disabled="!!pending" @click="close">Dismiss</button>
      <button class="btn primary" type="button" :disabled="!!pending || !!block || tool !== review.tool" @click="accept">
        {{ pending ? 'Saving…' : review.calibration ? 'Save calibration' : 'Apply length' }}
      </button>
    </template>
  </ModalDialog>
</template>

<style scoped>
p { margin: 0; line-height: 1.5; }
.setter-error { color: var(--danger); }
.setter-values { display: grid; grid-template-columns: 1fr auto; gap: 8px 16px; margin: 0; }
.setter-values dd { margin: 0; text-align: right; }
</style>
