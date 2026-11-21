<script lang="ts">
import { reactive } from 'vue'

/** Kept while CoffeeCNC runs, so that a return to Manual finds the same feed. mm and mm/min. */
export const powerFeed = reactive<{ extent: 'limit' | 'distance'; distance: number; feed: number }>({ extent: 'limit', distance: 10, feed: 200 })
</script>

<script setup lang="ts">
// Power feed: one feed move along one axis in machine coordinates, to just inside the soft limit
// or a set distance. A tap on a direction shows the target and feed; Start feed sends the MDI.
import { computed, ref, shallowRef, watch } from 'vue'

import { useCardFeedback } from '../card-feedback'
import { mdiBlock, panel, send } from '../machine'
import { limitMarginMm, powerFeedPlan, type PowerFeedPlan } from '../manual-moves'
import { parseNumericExpression } from '../numeric-expression'
import { vNumeric } from '../numeric-input'
import type { AxisLetter } from '../protocol'
import type { SetterStatusLine } from '../setter-state'
import { touchProbeLoaded } from '../tool-models'
import { vTooltip } from '../tooltip'
import { formatLength, formatVelocity, fromDisplay, toDisplay, velocityUnit } from '../units'
import AppIcon from './AppIcon.vue'
import ModalDialog from './ModalDialog.vue'

const emit = defineEmits<{ line: [SetterStatusLine] }>()

const unit = computed(() => panel.unit)
const unitName = computed(() => unit.value === 'inch' ? 'in' : 'mm')
const maxFeed = computed(() => panel.hello?.max_jog_velocity ?? 0)

const pad: readonly (readonly [AxisLetter, -1 | 1, string, 'up' | 'down' | 'left' | 'right'])[] = [
  ['y', 1, 'ypos', 'up'],
  ['x', -1, 'xneg', 'left'],
  ['x', 1, 'xpos', 'right'],
  ['y', -1, 'yneg', 'down'],
  ['z', 1, 'zpos', 'up'],
  ['z', -1, 'zneg', 'down'],
]

const directionLabel = (axis: AxisLetter, direction: -1 | 1): string => `${axis.toUpperCase()}${direction > 0 ? '+' : '−'}`

const shown = (mm: number, decimals: number): string => String(Number(toDisplay(mm, unit.value).toFixed(decimals)))
const distanceText = ref(shown(powerFeed.distance, 4))
const feedText = ref(shown(powerFeed.feed, 1))
watch(unit, () => {
  distanceText.value = shown(powerFeed.distance, 4)
  feedText.value = shown(powerFeed.feed, 1)
})

function readLength(text: string): number | null {
  const value = parseNumericExpression(text, unit.value)
  return value === null ? null : fromDisplay(value, unit.value)
}

function commitDistance(): void {
  const value = readLength(distanceText.value)
  if (value !== null && value > 0) powerFeed.distance = value
  distanceText.value = shown(powerFeed.distance, 4)
}

function commitFeed(): void {
  const value = readLength(feedText.value)
  if (value !== null && value > 0) powerFeed.feed = Math.min(value, maxFeed.value || value)
  feedText.value = shown(powerFeed.feed, 1)
}

const probeBlock = computed(() => touchProbeLoaded.value ? 'A touch probe is loaded' : null)
const block = computed(() => mdiBlock.value ?? probeBlock.value)

function plan(axis: AxisLetter, direction: -1 | 1): ReturnType<typeof powerFeedPlan> {
  return powerFeedPlan(
    { status: panel.status, machineBlock: mdiBlock.value, probeLoaded: touchProbeLoaded.value },
    { axis, direction, feed: powerFeed.feed, maxFeed: maxFeed.value,
      extent: powerFeed.extent === 'limit' ? { kind: 'limit' } : { kind: 'distance', distance: powerFeed.distance } },
  )
}

const extentText = computed(() => powerFeed.extent === 'limit'
  ? 'to the soft limit'
  : `${formatLength(powerFeed.distance, unit.value)} ${unitName.value}`)
const { line, allow } = useCardFeedback(
  () => '',
  () => block.value,
  [block, (): unknown => powerFeed.extent, (): number => powerFeed.distance, (): number => powerFeed.feed],
)
watch(line, (value) => { emit('line', value) }, { immediate: true })

const confirm = shallowRef<PowerFeedPlan | null>(null)

function choose(axis: AxisLetter, direction: -1 | 1): void {
  const planned = plan(axis, direction)
  if (!allow(planned.ok ? null : planned.reason) || !planned.ok) return
  confirm.value = planned.plan
}

function start(): void {
  const shownPlan = confirm.value
  confirm.value = null
  if (shownPlan === null) return
  // The machine may have changed while the confirmation was open; send only what was shown.
  const planned = plan(shownPlan.axis, shownPlan.direction)
  if (!allow(planned.ok ? null : planned.reason) || !planned.ok) return
  if (planned.plan.text !== shownPlan.text) { allow('The machine moved. Tap the direction again'); return }
  send({ type: 'mdi', text: planned.plan.text })
}

function stop(): void {
  send({ type: 'abort' })
}
</script>

<template>
  <div class="feed-controls">
    <div class="pad">
      <button
        v-for="[axis, direction, area, icon] in pad"
        :key="area"
        v-tooltip="block ?? `Power feed ${directionLabel(axis, direction)} ${extentText}`"
        class="btn jogbtn"
        type="button"
        :style="{ gridArea: area }"
        :aria-disabled="block !== null"
        @click="choose(axis, direction)"
      >
        <AppIcon :name="icon" />
        <span class="mono">{{ directionLabel(axis, direction) }}</span>
      </button>
      <button v-tooltip="'Stop the feed (same as Stop)'" class="btn danger jogbtn stopfeed" type="button" @click="stop">
        Stop
      </button>
    </div>

    <div class="extent">
      <div class="choices" role="group" aria-label="Feed extent">
        <button v-tooltip="`Stop ${limitMarginMm} mm inside the soft limit`" class="btn" type="button" :class="{ on: powerFeed.extent === 'limit' }" :aria-pressed="powerFeed.extent === 'limit'" @click="powerFeed.extent = 'limit'">
          To limit
        </button>
        <button v-tooltip="'Feed a set distance'" class="btn" type="button" :class="{ on: powerFeed.extent === 'distance' }" :aria-pressed="powerFeed.extent === 'distance'" @click="powerFeed.extent = 'distance'">
          Distance
        </button>
      </div>
      <label class="entry">
        <span class="label">Feed</span>
        <input
          v-model="feedText"
          v-numeric
          class="field mono"
          type="text"
          inputmode="decimal"
          autocomplete="off"
          aria-label="Power feed rate"
          :data-numeric-unit="unit"
          data-numeric-effect="Feed per minute of the next power feed."
          @change="commitFeed"
          @keydown.enter.prevent="commitFeed"
        />
        <span class="hint">{{ velocityUnit(unit) }}</span>
      </label>
      <label class="entry" :class="{ hidden: powerFeed.extent !== 'distance' }">
        <span class="label">Travel</span>
        <input
          v-model="distanceText"
          v-numeric
          class="field mono"
          type="text"
          inputmode="decimal"
          autocomplete="off"
          aria-label="Power feed distance"
          :disabled="powerFeed.extent !== 'distance'"
          :data-numeric-unit="unit"
          data-numeric-effect="Distance of the next power feed."
          @change="commitDistance"
          @keydown.enter.prevent="commitDistance"
        />
        <span class="hint">{{ unitName }}</span>
      </label>
    </div>
  </div>

  <ModalDialog v-if="confirm" :title="`Power feed ${directionLabel(confirm.axis, confirm.direction)}?`" size="small" fresh-gesture @close="confirm = null">
    <p class="coordinate mono">{{ confirm.axis.toUpperCase() }} {{ formatLength(confirm.start, unit) }} → {{ formatLength(confirm.target, unit) }} {{ unitName }}</p>
    <p class="coordinate mono">F {{ formatVelocity(confirm.feed, unit) }} {{ velocityUnit(unit) }}</p>
    <p>Machine coordinates. {{ powerFeed.extent === 'limit' ? `Stops ${limitMarginMm} mm inside the soft limit.` : 'Stays inside the soft limits.' }}</p>
    <template #footer>
      <button class="btn wide" type="button" @click="confirm = null">Cancel</button>
      <button class="btn primary wide" type="button" @click="start">Start feed</button>
    </template>
  </ModalDialog>
</template>

<style scoped>
.feed-controls { display: grid; grid-template-columns: 300px minmax(0, 1fr); gap: 12px; align-items: center; }
.pad {
  display: grid;
  justify-self: center;
  grid-template-areas:
    '. ypos . gap zpos'
    'xneg stop xpos gap .'
    '. yneg . gap zneg';
  grid-template-columns: 68px 68px 68px 12px 68px;
  grid-template-rows: 68px 68px 68px;
  gap: 4px;
}
.jogbtn { flex-direction: column; gap: 0; padding: 0; font-size: 20px; }
.stopfeed { grid-area: stop; font-size: 13px; }
.extent { display: flex; flex-direction: column; gap: 6px; min-width: 0; }
.choices { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 4px; }
.choices .btn { padding-inline: 8px; }
.entry { display: grid; grid-template-columns: 64px minmax(44px, 1fr) 46px; align-items: center; gap: 6px; }
.entry.hidden { visibility: hidden; }
.entry .field { width: 100%; min-width: 0; padding-inline: 8px; text-align: right; }
.entry .hint { white-space: nowrap; }
.btn[aria-disabled="true"] { opacity: .68; }
.coordinate { margin: 0; font-size: 20px; }
p { margin: 0; }
.wide { min-height: 48px; }
</style>
