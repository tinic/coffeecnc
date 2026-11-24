<script setup lang="ts">
// The HAL diagnostics view (#251): each joint's limit and home switches, the E-stop chain, the
// probe, the spindle and the machine's own configured signals, live while the view is open; and
// the HAL tools, each as a window of its own. Opening the view asks the machine for readings,
// closing it ends them: HAL is read only while someone looks.
import { computed, onBeforeUnmount, onMounted, ref, shallowRef, watch } from 'vue'
import { useI18n } from 'vue-i18n'

import { alarms, groups, shown, stateTips, type Shown } from '../diagnostics'
import { onResult, panel, send } from '../machine'
import type { CommandBody, DiagnosticInput, HalToolName } from '../protocol'
import { translateMessage } from '../i18n/messages'
import { vTooltip } from '../tooltip'
import ModalDialog from './ModalDialog.vue'

const { t } = useI18n()
const emit = defineEmits<{ close: [] }>()

/** A reading older than this is shown as not current: the feed sends about ten a second. */
const staleMs = 1000

const starting = ref<HalToolName | null>(null)
const refused = shallowRef<(() => string) | null>(null)
const refusal = computed(() => refused.value?.() ?? '')
let request: CommandBody | null = null
const removeListener = onResult((command, result) => {
  if (command !== request) return
  request = null
  starting.value = null
  refused.value = result.outcome === 'accepted' ? null : (): string => translateMessage(result.reason, result.refusal)
})

const now = ref(Date.now())
const receivedAt = ref(0)
let clock: number | null = null
// The page's last reading is from an earlier opening: start from nothing.
panel.diagnostics = null
watch(() => panel.diagnostics, (reading) => { if (reading !== null) receivedAt.value = Date.now() })
onMounted(() => {
  send({ type: 'watch_diagnostics', on: true })
  clock = window.setInterval(() => { now.value = Date.now() }, 250)
})
onBeforeUnmount(() => {
  if (clock !== null) window.clearInterval(clock)
  send({ type: 'watch_diagnostics', on: false })
  removeListener()
})

const reading = computed(() => panel.diagnostics)
const sides = computed(() => [
  { key: 'estop', title: t('status.diagnostics.section.estop'), rows: sections.value.estop, hint: null },
  { key: 'probe', title: t('status.diagnostics.section.probe'), rows: sections.value.probe, hint: t('status.diagnostics.setterHint') },
  { key: 'spindle', title: t('status.diagnostics.section.spindle'), rows: sections.value.spindle, hint: null },
])
const sections = computed(() => groups(reading.value))
const stale = computed(() => reading.value !== null && now.value - receivedAt.value > staleMs)
const alarmCount = computed(() => alarms(reading.value))

function value(input: DiagnosticInput | null): Shown | null {
  return input === null ? null : shown(input)
}
function words(input: DiagnosticInput | null): string {
  const display = value(input)
  if (display === null) return '—'
  return display.key === null ? display.text : t(display.key)
}
function note(input: DiagnosticInput | null): string | null {
  const key = value(input)?.note
  return key === null || key === undefined ? null : t(key)
}
function tip(input: DiagnosticInput | null): string {
  if (input === null) return ''
  const key = stateTips[input.state]
  const where = t(input.kind === 'signal' ? 'status.diagnostics.signalName' : 'status.diagnostics.pinName', { name: input.name })
  return key === null ? where : `${where} · ${t(key)}`
}
function tone(input: DiagnosticInput | null): string {
  return value(input)?.tone ?? 'idle'
}

const labels: Record<string, string> = {
  estop_chain: 'status.diagnostics.row.estopChain',
  estop_request: 'status.diagnostics.row.estopRequest',
  probe: 'status.diagnostics.row.probe',
  spindle_on: 'status.diagnostics.row.spindleOn',
  spindle_at_speed: 'status.diagnostics.row.spindleAtSpeed',
  spindle_speed_out: 'status.diagnostics.row.spindleSpeedOut',
  spindle_speed_in: 'status.diagnostics.row.spindleSpeedIn',
  spindle_fault: 'status.diagnostics.row.spindleFault',
}
function label(input: DiagnosticInput): string {
  if (input.role === 'configured') return input.label ?? input.name
  const key = labels[input.role]
  return key === undefined ? input.name : t(key)
}

/** INI syntax, the same in every language. */
const example = 'DIAGNOSTIC = E-stop button | estop-ext' // i18n-ignore: INI syntax

const toolTips: Record<HalToolName, string> = {
  halshow: 'status.diagnostics.tools.halshowTip',
  halmeter: 'status.diagnostics.tools.halmeterTip',
  halscope: 'status.diagnostics.tools.halscopeTip',
}
const toolNames: readonly HalToolName[] = ['halshow', 'halmeter', 'halscope']
const tools = computed(() => toolNames.map((tool) => {
  const state = reading.value?.tools.find((candidate) => candidate.tool === tool)
  return { tool, available: state?.available ?? false, open: state?.open ?? false, known: state !== undefined }
}))
function launch(tool: HalToolName): void {
  if (starting.value !== null) return
  refused.value = null
  starting.value = tool
  const command: CommandBody = { type: 'launch_hal_tool', tool }
  // Before send: the browser simulator answers at once.
  request = command
  send(command)
}
function toolState(tool: { available: boolean; open: boolean; known: boolean }): string {
  if (!tool.known) return t('status.diagnostics.tools.unknown')
  if (!tool.available) return t('status.diagnostics.tools.notInstalled')
  return tool.open ? t('status.diagnostics.tools.open') : t('status.diagnostics.tools.closed')
}
</script>

<template>
  <ModalDialog :title="t('status.diagnostics.title')" size="wide" expanded @close="emit('close')">
    <template #header>
      <p class="summary" role="status">
        <span v-if="reading === null">{{ t('status.diagnostics.waiting') }}</span>
        <span v-else-if="stale" class="stale">{{ t('status.diagnostics.stale') }}</span>
        <span v-else-if="alarmCount > 0" class="alarm">{{ t('status.diagnostics.alarms', { count: alarmCount }, alarmCount) }}</span>
        <span v-else class="live"><i class="dot"></i>{{ t('status.diagnostics.live') }}</span>
      </p>
    </template>
    <p class="hint">{{ t('status.diagnostics.hint') }}</p>
    <div class="grid" :class="{ dim: stale || reading === null }">
      <div class="column">
        <section>
          <h3>{{ t('status.diagnostics.section.switches') }}</h3>
          <table>
            <thead>
              <tr>
                <th>{{ t('status.diagnostics.column.joint') }}</th>
                <th>{{ t('status.diagnostics.column.minLimit') }}</th>
                <th>{{ t('status.diagnostics.column.maxLimit') }}</th>
                <th>{{ t('status.diagnostics.column.homeSwitch') }}</th>
                <th>{{ t('status.diagnostics.column.driveFault') }}</th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="row in sections.joints" :key="row.joint">
                <th scope="row"><span class="axis mono">{{ row.axis?.toUpperCase() ?? '' }}</span><span class="joint">{{ t('status.diagnostics.jointName', { joint: row.joint }) }}</span></th>
                <td v-for="cell in [row.min, row.max, row.home, row.fault]" :key="cell?.name ?? ''">
                  <span v-tooltip="tip(cell)" class="reading" :class="tone(cell)"><i class="dot"></i>{{ words(cell) }}</span>
                  <span v-if="note(cell)" class="note">{{ note(cell) }}</span>
                </td>
              </tr>
            </tbody>
          </table>
          <p v-if="reading !== null && sections.joints.length === 0" class="hint">{{ t('status.diagnostics.noJoints') }}</p>
        </section>
        <section>
          <h3>{{ t('status.diagnostics.section.configured') }}</h3>
          <ul v-if="sections.configured.length > 0" class="rows">
            <li v-for="input in sections.configured" :key="input.label">
              <span class="row-label">{{ label(input) }}<span class="hal-name mono">{{ input.name }}</span></span>
              <span v-if="note(input)" class="note">{{ note(input) }}</span>
              <span v-tooltip="tip(input)" class="reading" :class="tone(input)"><i class="dot"></i>{{ words(input) }}</span>
            </li>
          </ul>
          <p v-else-if="reading !== null" class="hint">{{ t('status.diagnostics.noConfigured', { example }) }}</p>
        </section>
      </div>
      <div class="column">
        <section v-for="side in sides" :key="side.key">
          <h3>{{ side.title }}</h3>
          <ul class="rows">
            <li v-for="input in side.rows" :key="input.name">
              <span class="row-label">{{ label(input) }}<span class="hal-name mono">{{ input.name }}</span></span>
              <span v-if="note(input)" class="note">{{ note(input) }}</span>
              <span v-tooltip="tip(input)" class="reading" :class="tone(input)"><i class="dot"></i>{{ words(input) }}</span>
            </li>
          </ul>
          <p v-if="side.hint" class="hint">{{ side.hint }}</p>
        </section>
      </div>
    </div>
    <template #footer>
      <div class="tools">
        <span class="tools-title">{{ t('status.diagnostics.section.tools') }}</span>
        <button
          v-for="tool in tools" :key="tool.tool" v-tooltip="t(toolTips[tool.tool])" class="btn tool" type="button"
          :disabled="!tool.available || tool.open || starting !== null" @click="launch(tool.tool)"
        >
          <span class="mono">{{ tool.tool }}</span><span class="tool-state">{{ starting === tool.tool ? t('status.diagnostics.tools.starting') : toolState(tool) }}</span>
        </button>
        <p v-if="refusal" class="refusal" role="alert">{{ refusal }}</p>
        <p v-else class="hint">{{ t('status.diagnostics.tools.hint') }}</p>
      </div>
    </template>
  </ModalDialog>
</template>

<style scoped>
.summary { flex: 1; margin: 0; font-size: var(--type-body); color: var(--text-dim); text-align: right; }
.summary .live { display: inline-flex; align-items: center; gap: 6px; color: var(--go); }
.summary .live .dot { background: var(--go); }
.summary .alarm { color: var(--stop); font-weight: 600; }
.summary .stale { color: var(--warn); font-weight: 600; }
.hint { margin: 0; font-size: var(--type-sm); line-height: 1.4; color: var(--text-dim); }
.grid { display: grid; grid-template-columns: minmax(0, 1.25fr) minmax(0, 1fr); gap: 20px; align-items: start; }
.grid.dim .reading { opacity: .5; }
.column { display: grid; gap: 12px; min-width: 0; }
section { min-width: 0; }
h3 { margin: 0 0 4px; font-size: var(--type-md); font-weight: 600; }
table { width: 100%; border-collapse: collapse; font-size: var(--type-body); }
th, td { padding: 4px 6px; border-bottom: 1px solid var(--line); text-align: left; vertical-align: top; }
thead th { font-size: var(--type-sm); font-weight: 500; color: var(--text-dim); }
tbody th { white-space: nowrap; font-weight: 500; }
.axis { display: inline-block; min-width: 18px; font-size: var(--type-md); font-weight: 600; }
.joint { margin-left: 4px; font-size: var(--type-sm); color: var(--text-dim); font-weight: 400; }
.rows { margin: 0; padding: 0; list-style: none; }
.rows li { display: flex; align-items: baseline; gap: 8px; min-height: 28px; padding: 4px 0; border-bottom: 1px solid var(--line); font-size: var(--type-body); }
.row-label { display: flex; flex: 1 1 auto; flex-wrap: wrap; align-items: baseline; column-gap: 8px; min-width: 0; }
.hal-name { font-size: var(--type-sm); color: var(--text-faint); overflow-wrap: anywhere; }
.reading { display: inline-flex; flex: none; align-items: center; gap: 6px; font-weight: 600; white-space: nowrap; }
.dot { display: inline-block; flex: none; width: 9px; height: 9px; border-radius: 50%; background: var(--line-strong); }
.reading.go { color: var(--go); }.reading.go .dot { background: var(--go); }
.reading.stop { color: var(--stop); }.reading.stop .dot { background: var(--stop); box-shadow: 0 0 0 3px var(--stop-soft); }
.reading.warn { color: var(--warn); }.reading.warn .dot { background: var(--warn); box-shadow: 0 0 0 3px var(--warn-soft); }
.reading.active { color: var(--accent); }.reading.active .dot { background: var(--accent); }
.reading.idle { color: var(--text-dim); font-weight: 500; }.reading.idle .dot { background: var(--text-faint); opacity: .55; }
.note { flex: none; font-size: var(--type-sm); color: var(--text-faint); white-space: nowrap; }
td .note { display: block; }
.tools { display: flex; flex: 1; flex-wrap: wrap; align-items: center; gap: 8px; min-width: 0; }
.tools-title { font-size: var(--type-body); font-weight: 600; }
.tool { display: flex; flex-direction: column; align-items: flex-start; gap: 1px; min-width: 120px; min-height: 48px; padding: 4px 12px; }
.tool .mono { font-size: var(--type-body); font-weight: 600; }
.tool-state { font-size: var(--type-sm); color: var(--text-dim); }
.tools .hint, .refusal { flex: 1 1 220px; margin: 0; font-size: var(--type-sm); }
.refusal { color: var(--stop); }
@media (max-width: 900px) { .grid { grid-template-columns: 1fr; } }
</style>
