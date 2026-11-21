<script setup lang="ts">
import type { LengthUnit, ProgramCheck } from '../protocol'
import { formatLength, formatVelocity, toolLabel, velocityUnit, wcsName } from '../units'
import AppIcon from './AppIcon.vue'
import ModalDialog from './ModalDialog.vue'

const props = defineProps<{
  check: ProgramCheck | null
  unit: LengthUnit
  maxRpm?: number | undefined
  canConfirm?: boolean
  changed?: boolean
  breakpointLine?: number | null
}>()
const emit = defineEmits<{ close: []; confirm: [] }>()

const rpm = (value: number): string => Math.round(value).toLocaleString('en-US')
const length = (value: number): string => formatLength(value, props.unit)
function range(min: number, max: number, format: (value: number) => string): string {
  return min === max ? format(min) : `${format(min)} to ${format(max)}`
}
</script>

<template>
  <ModalDialog title="Program check" size="medium" fresh-gesture @close="emit('close')">
    <p v-if="changed" class="notice" role="status">The program check changed. Close and review it again before starting.</p>
    <p v-else-if="!check" class="notice" role="status">Checking the loaded program…</p>
    <template v-else>
      <p v-if="breakpointLine" class="breakpoint-notice">Pause before line {{ breakpointLine }}</p>
      <div v-if="check.errors.length || check.warnings.length" class="issues" :class="check.errors.length ? 'blocked' : 'review'">
        <AppIcon name="warning" />
        <div>
          <strong>{{ check.errors.length ? 'Cycle start blocked' : 'Review before starting' }}</strong>
          <ul>
            <li v-for="issue in check.errors" :key="issue">{{ issue }}</li>
            <li v-for="issue in check.warnings" :key="issue">{{ issue }}</li>
          </ul>
        </div>
      </div>
      <dl class="metrics">
        <div><dt>Machine travel</dt><dd>{{ check.travel === 'fits' ? 'Fits' : check.travel === 'outside' ? 'Outside limits' : 'Unverified' }}</dd></div>
        <div><dt>Program units</dt><dd>{{ check.units.map((u) => u === 'mm' ? 'G21 · mm' : 'G20 · inch').join(' / ') || '—' }}</dd></div>
        <div><dt>Feed</dt><dd class="mono">{{ check.feed ? `${range(check.feed.min, check.feed.max, (v) => formatVelocity(v, unit))} ${velocityUnit(unit)}` : '—' }}</dd></div>
        <div><dt>Spindle</dt><dd class="mono">{{ check.spindle ? `${range(check.spindle.min, check.spindle.max, rpm)} rpm` : '—' }}<small v-if="maxRpm">Limit {{ rpm(maxRpm) }} rpm</small></dd></div>
      </dl>
      <section v-if="check.frames.length" class="frame-list">
        <h3>Work extents <small>{{ unit }}</small></h3>
        <div v-for="frame in check.frames" :key="frame.wcs" class="frame">
          <strong>{{ wcsName(frame.wcs) }}</strong>
          <span v-if="!frame.available" class="unverified">Origin unverified</span>
          <template v-else-if="frame.bounds">
            <span><small>X</small> {{ range(frame.bounds.min[0], frame.bounds.max[0], length) }}</span>
            <span><small>Y</small> {{ range(frame.bounds.min[1], frame.bounds.max[1], length) }}</span>
            <span><small>Lowest Z</small> {{ length(frame.bounds.min[2]) }}</span>
          </template>
        </div>
      </section>
      <section v-if="check.tools.length" class="tool-list">
        <h3>Tool lengths</h3>
        <ul>
          <li v-for="tool in check.tools" :key="tool.tool" :class="{ unverified: tool.length === 'missing' }">
            <strong>{{ toolLabel(tool.tool) }}</strong>
            {{ tool.length === 'touched_off' ? 'Touched off' : tool.length === 'table' ? 'From table' : 'Missing' }}
          </li>
        </ul>
      </section>
    </template>
    <template #footer>
      <button class="btn" type="button" @click="emit('close')">{{ canConfirm ? 'Cancel' : 'Done' }}</button>
      <button v-if="canConfirm && check && !check.errors.length && !changed" class="btn primary" type="button" @click="emit('confirm')">
        <AppIcon name="play" />{{ breakpointLine ? `Run to line ${String(breakpointLine)}` : 'Run program' }}
      </button>
    </template>
  </ModalDialog>
</template>

<style scoped>
.issues { display: flex; gap: 10px; padding: 12px; border: 1px solid var(--line-strong); border-radius: 9px; background: var(--bg-sunken); font-size: 13px; }
.breakpoint-notice { margin: 0; font-size: 13px; color: var(--warn); }
.issues.review { border-color: var(--warn); background: var(--warn-soft); }
.issues.review > .icon { color: var(--warn); }
.issues.blocked { border-color: var(--stop); background: var(--stop-soft); }
.issues.blocked > .icon, .unverified { color: var(--stop); }
.issues ul { display: grid; gap: 4px; margin: 6px 0 0; padding-left: 17px; }
.metrics { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); flex-shrink: 0; gap: 1px; border: 1px solid var(--line); border-radius: 9px; background: var(--line); overflow: hidden; margin: 0; }
.metrics > div { display: flex; align-items: baseline; flex-wrap: wrap; justify-content: space-between; gap: 6px; padding: 10px; background: var(--panel); }
dt, small, .notice { color: var(--text-dim); }
dt, dd { margin: 0; font-size: 13px; }
dd > small { display: block; margin-top: 3px; font-size: 10px; }
h3 { font-size: 12px; margin: 0 0 7px; font-weight: 600; }
h3 > small { margin-left: 4px; font-weight: 400; }
.frame { display: grid; grid-template-columns: max-content repeat(3, 1fr); gap: 6px 12px; padding: 9px 0; border-top: 1px solid var(--line); font-size: 12px; }
.frame small { display: block; font-size: 10px; margin-bottom: 3px; }
.tool-list ul { display: flex; flex-wrap: wrap; gap: 6px; margin: 0; padding: 0; }
.tool-list li { list-style: none; padding: 5px 8px; border: 1px solid var(--line); border-radius: 6px; font-size: 12px; }
.tool-list strong { margin-right: 4px; }
.notice { margin: 0; font-size: 13px; }
</style>
