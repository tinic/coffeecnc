<script setup lang="ts">
import { computed, nextTick, ref, useTemplateRef, watch } from 'vue'
import { chooseProgramLayout, findTool, live, panel, reported } from '../machine'
import { useDrawnPreview } from '../drawn-preview'
import { cycleTime, formatDuration, remainingFrom, sectionSeconds, velocityLimits } from '../cycle-time'
import { elapsedSeconds, runClock } from '../run-clock'
import { machineBlock } from '../machine-block'
import { currentProgramPreview, describeProgramCheck } from '../program-check'
import type { ProgramLayout } from '../store'
import { formatLength, formatVelocity, toolLabel, velocityUnit } from '../units'
import AppIcon from './AppIcon.vue'
import NcText from './NcText.vue'
import PathView from './PathView.vue'
import PartModel from './PartModel.vue'
import RestartDialog from './RestartDialog.vue'
import ProgramCheckDialog from './ProgramCheckDialog.vue'
import SetterStatusBand from './SetterStatusBand.vue'
import MachineFeedback from './MachineFeedback.vue'

const status = reported
const text = useTemplateRef<InstanceType<typeof NcText>>('text')
const selected = ref<number | null>(null)
const checkOpen = ref(false)
const check = computed(() => describeProgramCheck(currentProgramPreview(panel.preview, status.value), panel.unit))
const checkLabel = computed(() => check.value === null ? 'Checking program'
  : check.value.errors.length ? 'Cycle start blocked'
  : check.value.warnings.length ? 'Review program warnings'
  : check.value.travel === 'unknown' ? 'Physical travel unverified' : 'Program check passed')
const program = computed(() => panel.program)
watch(() => panel.program?.revision, () => { selected.value = null })
const textCurrent = computed(() => program.value !== null && status.value !== null && program.value.file === status.value.program.file && program.value.revision === status.value.program.revision)
const previewCurrent = computed(() => panel.preview !== null && status.value !== null && panel.preview.context === status.value.preview_context)
const failure = computed(() => panel.preview?.state === 'failed' && previewCurrent.value ? panel.preview.error : null)
const unavailableReason = computed(() => panel.preview?.state === 'unavailable' && previewCurrent.value ? panel.preview.error?.text : undefined)
const drawn = useDrawnPreview(panel)
const failureLine = computed(() => textCurrent.value ? lineInText(failure.value?.line ?? 0) : 0)
const layouts: readonly [ProgramLayout, string][] = [['path', 'Preview'], ['text', 'Code'], ['split', 'Split']]
// These are tool sections reported by the interpreter, not inferred CAM operations.
const sections = computed(() => {
  const rows: { tool: number; first: number; last: number }[] = []
  for (const segment of drawn.value?.segments ?? []) {
    const last = rows.at(-1)
    if (last?.tool === segment.tool) last.last = Math.max(last.last, segment.line)
    else rows.push({ tool: segment.tool, first: segment.line, last: segment.line })
  }
  return rows
})
const currentTool = computed(() => selected.value ?? status.value?.tool.in_spindle ?? 0)
const tool = computed(() => findTool(currentTool.value))
const toolTitle = computed(() => {
  if (currentTool.value === 0) return 'No tool loaded'
  const label = toolLabel(currentTool.value)
  if (tool.value === null) return `${label} · Tool not in table`
  const comment = tool.value.comment.trim()
  return comment ? `${label} · ${comment}` : label
})
// LinuxCNC can retain counters after a run, through MDI, or across a new file load.
// Only present them as current program progress in a fresh AUTO execution context.
const programExecuting = computed(() => {
  const s = status.value
  return live.value && textCurrent.value && s !== null && s.machine === 'on' && s.mode === 'auto' &&
    !s.axes.some((axis) => axis.homing) && (s.interp !== 'idle' || s.wait.kind !== 'none')
})
const currentLine = computed(() => programExecuting.value ? status.value?.program.current_line ?? 0 : 0)
const motionLine = computed(() => programExecuting.value ? status.value?.program.motion_line ?? 0 : 0)
function lineInText(line: number): number {
  return Number.isSafeInteger(line) && line > 0 && line <= (program.value?.lines.length ?? 0) ? line : 0
}
const lineLabel = computed(() => {
  if (!textCurrent.value || program.value === null) return '—'
  const count = program.value.lines.length
  const line = currentLine.value
  if (Number.isSafeInteger(line) && line > 0) {
    // Do not infer full-file progress from a partial prefix or an out-of-range counter.
    return `Line ${String(line)}${!program.value.truncated && line <= count ? ` / ${String(count)}` : ''}`
  }
  return `${String(count)} ${count === 1 ? 'line' : 'lines'}${program.value.truncated ? ' shown' : ''}`
})
const runState = computed(() => {
  const s = status.value
  if (s === null || machineBlock(panel) !== null) return ''
  if (s.wait.kind === 'tool_change') return `Waiting for tool ${String(s.wait.tool)}`
  if (s.wait.kind === 'dwell') return 'Dwelling'
  if (!s.motion.in_position || s.motion.velocity > 0) return 'Machine moving'
  return s.program.file ? 'Program loaded · machine idle' : 'No program loaded'
})
// Cycle time from the drawn preview: before the run the estimate, during it elapsed and left.
const time = computed(() => {
  const preview = drawn.value
  const limits = velocityLimits(panel.hello)
  return preview !== null && limits !== null && preview.state === 'ready' ? cycleTime(preview, limits) : null
})
const sectionTimes = computed(() => time.value === null ? [] : sectionSeconds(time.value, sections.value))
const timeLabel = computed(() => {
  const estimate = time.value
  const elapsed = elapsedSeconds.value
  if (elapsed !== null && programExecuting.value) {
    const line = motionLine.value > 0 ? motionLine.value : currentLine.value
    // Feeds scale with the override; rapids do not, so this stays an estimate.
    const override = Math.max(status.value?.motion.feed_override ?? 1, 0.05)
    const left = estimate !== null && line > 0 ? remainingFrom(estimate, line) / override : null
    return `Elapsed ${formatDuration(elapsed)}${left === null ? '' : ` · about ${formatDuration(left)} left`}`
  }
  if (estimate === null) return ''
  const changes = estimate.toolChanges
  const parts = [`Estimated ${formatDuration(estimate.total)}`]
  if (changes > 0) parts.push(`+ ${String(changes)} tool ${changes === 1 ? 'change' : 'changes'}`)
  if (runClock.lastSeconds !== null) {
    parts.push(`last ${formatDuration(runClock.lastSeconds)}`, `${String(runClock.parts)} ${runClock.parts === 1 ? 'part' : 'parts'}`)
  }
  return parts.join(' · ')
})
const timeNote = computed(() => time.value?.unknownFeeds === true
  ? 'Some feeds are not per minute (G93/G95) and are counted at full speed.'
  : 'At 100 % overrides, without acceleration, tool changes or pauses.')

// A restart is planned from an idle, loaded program whose text is the one the machine has.
const canRestart = computed(() => textCurrent.value && !programExecuting.value && status.value?.interp === 'idle' &&
  sections.value.length > 0)
const restartAt = ref<{ line: number; section: string } | null>(null)
function sectionName(index: number): string {
  const section = sections.value[index]
  return section === undefined ? '' : `Section ${String(index + 1)} · ${toolLabel(section.tool)}`
}
function restartLine(line: number): void {
  let index = -1
  sections.value.forEach((section, candidate) => { if (section.first <= line) index = candidate })
  restartAt.value = { line, section: index < 0 ? 'Before the first tool section' : sectionName(index) }
}
// Where the program stopped before its end; offered only while a restart is possible.
const stoppedAt = computed(() => canRestart.value ? status.value?.program.restart_hint ?? null : null)
function restartSection(index: number): void {
  const section = sections.value[index]
  if (section !== undefined) restartAt.value = { line: section.first, section: sectionName(index) }
}
async function showFailure(): Promise<void> {
  if (failureLine.value === 0) return
  panel.programLayout = 'text'
  await nextTick()
  if (failureLine.value > 0) text.value?.goTo(failureLine.value)
}
</script>

<template>
  <div class="program">
    <div class="workspace-card">
      <section class="scene">
        <header class="scene-top">
          <span class="scene-title"><AppIcon :name="panel.programLayout === 'text' ? 'file' : 'fit'" />{{ panel.programLayout === 'text' ? 'NC program' : 'Toolpath preview' }}</span>
          <div class="scene-actions">
            <button
              v-if="status?.program.file" type="button" class="btn program-check"
              :title="checkLabel" :aria-label="`Program check · ${checkLabel}`"
              :class="{ blocked: check?.errors.length, review: check?.warnings.length }"
              @click="checkOpen = true"
            >
              <AppIcon :name="check?.errors.length || check?.warnings.length ? 'warning' : check ? 'check' : 'list'" />
              Program check
            </button>
            <div class="view-switch" aria-label="Program view"><button v-for="[layout, label] in layouts" :key="layout" type="button" :aria-pressed="panel.programLayout === layout" @click="chooseProgramLayout(layout)">{{ label }}</button></div>
          </div>
        </header>
        <div v-if="textCurrent && program?.truncated" class="failure" role="status"><AppIcon name="warning" /><span>Partial program text · Only the first {{ program.lines.length }} lines are shown. The controller runs the full file.</span></div>
        <div class="views" :class="panel.programLayout">
          <template v-if="panel.programLayout !== 'path'">
            <NcText v-if="program !== null && textCurrent" ref="text" :lines="program.lines" :motion-line="lineInText(motionLine)" :current-line="lineInText(currentLine)" :error-line="failureLine" :restartable="canRestart" @restart="restartLine" />
            <div v-else class="empty">{{ status?.program.file ? 'Waiting for program text' : 'Open a program to begin' }}</div>
          </template>
          <PathView v-if="panel.programLayout !== 'text'" />
          <div v-if="failure && (panel.programLayout === 'text' || failureLine > 0)" class="failure preview-diagnostic" role="status">
            <AppIcon name="warning" /><span>Preview stopped: {{ failure.text }}</span>
            <button v-if="failureLine > 0" type="button" @click="showFailure">Show line {{ failureLine }}</button>
          </div>
          <SetterStatusBand v-else-if="unavailableReason && panel.programLayout === 'text'" class="preview-diagnostic" :line="{ text: unavailableReason, level: 'blocked' }" />
          <div v-if="stoppedAt" class="stopped-at" role="status">
            <span>Stopped at line <strong class="mono">{{ stoppedAt.line }}</strong> · {{ toolLabel(stoppedAt.tool) }}</span>
            <button class="btn primary" type="button" @click="restartLine(stoppedAt.line)">Restart there</button>
          </div>
        </div>
      </section>
      <aside class="inspector">
        <div class="inspector-body">
          <div class="panel-head"><h2>Tool sequence</h2><span class="kicker">{{ sections.length }} sections</span></div>
          <div class="sections">
            <div v-for="(section, index) in sections" :key="`${section.first}-${section.tool}`" class="section-row">
              <button class="section" type="button" :aria-pressed="currentTool === section.tool" @click="selected = section.tool">
                <span class="section-number mono">{{ index + 1 }}</span>
                <span class="section-text"><strong>{{ section.tool === 0 ? 'No tool selected' : findTool(section.tool)?.comment || `Tool ${String(section.tool)}` }}</strong><small>T{{ section.tool }} · Lines {{ section.first }}–{{ section.last }}<template v-if="sectionTimes[index] !== undefined"> · {{ formatDuration(sectionTimes[index] ?? 0) }}</template></small></span>
              </button>
              <button v-if="canRestart" class="btn section-restart" type="button" :aria-label="`Restart ${sectionName(index)}`" @click="restartSection(index)">Restart</button>
            </div>
            <p v-if="sections.length === 0" class="quiet">{{ previewCurrent ? 'No tool sequence in this preview.' : 'Tool sequence unavailable.' }}</p>
          </div>
          <div class="tool">
            <div class="kicker">{{ currentTool === status?.tool.in_spindle ? 'Tool in spindle' : 'Inspected tool' }}</div>
            <div class="tool-title"><span class="tool-glyph"><AppIcon name="target" /></span><div><strong>{{ toolTitle }}</strong><small v-if="tool">Ø{{ formatLength(tool.diameter, panel.unit) }} {{ panel.unit }}</small></div></div>
            <div v-if="tool" class="kv"><span>Table Z offset</span><strong class="mono">{{ formatLength(tool.length, panel.unit) }} {{ panel.unit }}</strong></div>
            <div class="kv"><span>Spindle command</span><strong class="mono">{{ status?.spindle.direction === 'stopped' ? 'Stopped' : `${String(Math.round((status?.spindle.commanded_rpm ?? 0) * (status?.spindle.override ?? 1)))} rpm` }}</strong></div>
            <div class="kv"><span>Current feed</span><strong class="mono">{{ status ? formatVelocity(status.motion.velocity, panel.unit) : '—' }} {{ velocityUnit(panel.unit) }}</strong></div>
          </div>
        </div>
        <PartModel />
      </aside>
    </div>
    <RestartDialog v-if="restartAt" :line="restartAt.line" :section="restartAt.section" @close="restartAt = null" />
    <ProgramCheckDialog v-if="checkOpen" :check="check" :unit="panel.unit" :max-rpm="panel.hello?.max_spindle_rpm" @close="checkOpen = false" />
    <div class="status-row"><span>{{ runState }}</span><MachineFeedback class="program-feedback" /><span class="run-time mono" :title="timeNote">{{ timeLabel }}</span><span class="line-count mono">{{ lineLabel }}</span></div>
  </div>
</template>

<style scoped>
.program { display: flex; flex-direction: column; height: 100%; min-height: 0; gap: 14px; }
.workspace-card { display: grid; grid-template-columns: minmax(0, 1fr) 292px; flex: 1; min-height: 0; border: 1px solid var(--line); border-radius: 12px; background: var(--panel); overflow: hidden; }
.scene { display: flex; flex-direction: column; min-width: 0; min-height: 0; background: var(--bg-sunken); }
.scene-top { display: flex; align-items: center; justify-content: space-between; padding: 12px 18px 4px; gap: 12px; flex: none; }
.scene-title { display: flex; align-items: center; gap: 8px; font-size: 12px; font-weight: 500; }.scene-title :deep(svg) { width: 16px; }
.scene-actions { display: flex; align-items: center; gap: 9px; }
.program-check { min-height: 34px; padding: 5px 8px; white-space: nowrap; font-size: 11px; }
.program-check :deep(.icon) { width: 14px; height: 14px; }
.program-check.blocked { color: var(--stop); }
.program-check.review:not(.blocked) { color: var(--accent); }
.view-switch { display: flex; gap: 2px; }.view-switch button { border: 0; border-radius: 7px; padding: 8px 10px; font-size: 11px; color: var(--text-dim); background: none; cursor: pointer; }.view-switch button[aria-pressed=true] { color: var(--accent); background: var(--panel); }
.views { position: relative; flex: 1; display: grid; min-height: 0; min-width: 0; }.views.split { grid-template-columns: 1fr 1fr; }.views.text { padding: 8px 16px 16px; }
.views :deep(.nctext) { border: 0; border-radius: 0; }.empty { display: grid; place-items: center; color: var(--text-dim); }
.inspector { display: flex; flex-direction: column; min-height: 0; padding: 20px; border-left: 1px solid var(--line); overflow-y: auto; }
.panel-head { display: flex; align-items: baseline; justify-content: space-between; gap: 12px; margin-bottom: 12px; }h2 { margin: 0; font-size: 14px; font-weight: 600; letter-spacing: -.15px; }.kicker { font-size: 10px; letter-spacing: .8px; color: var(--text-dim); text-transform: uppercase; }
.sections { display: flex; flex-direction: column; gap: 5px; margin: 0 -8px 14px; }
.section-row { display: flex; align-items: center; gap: 6px; }.section-row .section { flex: 1; min-width: 0; }.section-restart { flex: none; height: 36px; padding: 0 10px; font-size: 12px; }
.section { display: flex; align-items: center; gap: 10px; min-height: 57px; padding: 10px 8px; border: 0; border-radius: 9px; background: none; text-align: left; cursor: pointer; }
.section[aria-pressed=true] { background: var(--accent-soft); }.section-number { display: grid; place-items: center; width: 25px; height: 25px; border: 1px solid var(--line); border-radius: 50%; color: var(--text-dim); font-size: 11px; flex: none; }
.section[aria-pressed=true] .section-number { border-color: color-mix(in srgb, var(--accent) 40%, transparent); color: var(--accent); }.section-text { display: flex; flex-direction: column; gap: 4px; }.section-text strong { font-weight: 500; font-size: 13px; }.section-text small { font-size: 11px; color: var(--text-dim); }
.tool { border-top: 1px solid var(--line); padding-top: 16px; margin-top: auto; }.tool-title { display: flex; align-items: center; gap: 10px; margin: 12px 0 16px; }.tool-title strong { font-size: 13px; font-weight: 500; }.tool-title small { display: block; margin-top: 4px; color: var(--text-dim); font-size: 11px; }.tool-glyph { display: flex; padding: 10px; background: var(--bg-sunken); border-radius: 8px; color: var(--text-dim); }
.kv { display: flex; justify-content: space-between; gap: 8px; margin: 10px 0; font-size: 12px; color: var(--text-dim); }.kv strong { flex: none; min-width: 14ch; text-align: right; color: var(--text); font-weight: 500; }.kv .mono { font-size: 11px; }
.context-note, .quiet { margin: 15px 0 0; font-size: 11px; color: var(--text-dim); }
.status-row { flex: none; min-height: 20px; display: grid; grid-template-columns: minmax(0, 1fr) auto 46ch 24ch; align-items: center; gap: 12px; font-size: 12px; }.status-row > .mono { font-size: 10px; color: var(--text-dim); }.line-count { min-width: 24ch; text-align: right; white-space: nowrap; }.run-time { text-align: right; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; font-size: 10px; color: var(--text-dim); }.warning { color: var(--stop); }
.status-row .program-feedback { margin: 0; }
.failure { display: flex; align-items: center; gap: 8px; padding: 8px 18px; color: var(--stop); font-size: 12px; }.failure button { border: 0; background: none; color: inherit; text-decoration: underline; cursor: pointer; flex: none; }.failure span { flex: 1; min-width: 0; overflow-wrap: anywhere; }
.preview-diagnostic { position: absolute; z-index: 1; inset: 8px 16px auto; margin: 0; pointer-events: none; }
.failure.preview-diagnostic { border: 1px solid var(--stop); border-radius: 9px; background: var(--stop-soft); }
.preview-diagnostic button { pointer-events: auto; }
/* Over the views, bottom left, so it never moves the code or the preview. */
.stopped-at { position: absolute; z-index: 1; left: 16px; bottom: 12px; display: flex; align-items: center; gap: 12px; padding: 6px 6px 6px 14px; border: 1px solid var(--accent); border-radius: 10px; background: var(--panel-raised); font-size: 13px; box-shadow: 0 4px 14px rgb(0 0 0 / 12%); }
.stopped-at .btn { height: 36px; }
@media (max-height: 850px) { .program { gap: 10px; }.inspector { padding: 14px 20px; }.section { min-height: 50px; }.tool-title { margin-block: 8px; }.kv { margin-block: 7px; }.context-note { margin-top: 10px; }.tool { padding-top: 12px; } }
.inspector { padding: 0; overflow: hidden; }
.inspector-body { display: flex; flex-direction: column; flex: 1; min-height: 0; padding: 20px; overflow-y: auto; }
.inspector-body > * { flex-shrink: 0; }
@media (max-height: 850px) { .inspector-body { padding: 14px 20px; } }
@media (max-height: 850px) {
  .inspector-body { padding-block: 10px; }
  .panel-head { margin-bottom: 6px; }
  .sections { gap: 0; margin-bottom: 8px; }
  .section { min-height: 44px; padding-block: 6px; }
  .tool { padding-top: 8px; }
  .tool-title { margin-block: 6px; }
  .kv { margin-block: 4px; }
}
</style>
