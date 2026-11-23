<script lang="ts">
import * as lineEditMachine from '../machine'
import { createProgramLineEdits } from '../program-line-edits'

// The clipboard and a pending save outlive a switch away from the Machine tab.
const lineEdits = createProgramLineEdits(lineEditMachine.panel, lineEditMachine)
</script>

<script setup lang="ts">
import { computed, nextTick, onBeforeUnmount, ref, useTemplateRef, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { allHomed, chooseProgramLayout, findTool, homingRequired, live, onResult, panel, reported, send } from '../machine'
import { useDrawnPreview } from '../drawn-preview'
import { cycleTime, formatDuration, progressAt, remainingFrom, sectionSeconds, velocityLimits } from '../cycle-time'
import { elapsedSeconds, runClock } from '../run-clock'
import { machineBlock } from '../machine-block'
import { breakpointLines as checkedBreakpointLines, currentProgramPreview, describeProgramCheck } from '../program-check'
import { breakpointSelection, selectedBreakpoint } from '../program-breakpoint'
import type { ProgramLayout } from '../store'
import { formatVelocity, toolLabel, velocityUnit } from '../units'
import AppIcon from './AppIcon.vue'
import { vTooltip } from '../tooltip'
import NcText from './NcText.vue'
import PathView from './PathView.vue'
import PartModel from './PartModel.vue'
import RestartDialog from './RestartDialog.vue'
import NumberDialog from './NumberDialog.vue'
import { rateWords, withRate, type RateWord } from '../program-edit'
import type { CommandBody } from '../protocol'
import ProgramCheckDialog from './ProgramCheckDialog.vue'
import SetterStatusBand from './SetterStatusBand.vue'
import { machineFeedback } from '../machine-feedback'
import { translateMessage } from '../i18n/messages'

const { t } = useI18n()

const props = defineProps<{ readOnly?: boolean }>()
const status = reported
const lineEditBar = computed(() => ({
  block: lineEdits.block.value,
  hasClipboard: lineEdits.hasClipboard.value,
  canUndo: lineEdits.canUndo.value,
  message: lineEdits.message.value,
}))
const text = useTemplateRef<InstanceType<typeof NcText>>('text')
const selected = ref<number | null>(null)
const checkOpen = ref(false)
const checkedPreview = computed(() => currentProgramPreview(panel.preview, status.value))
const check = computed(() => describeProgramCheck(checkedPreview.value, panel.unit))
const checkLabel = computed(() => check.value === null ? t('program.tab.checking')
  : check.value.errors.length ? t('program.tab.blocked')
  : check.value.warnings.length ? t('program.tab.review')
  : check.value.travel === 'unknown' ? t('program.tab.travelUnverified') : t('program.tab.passed'))
const program = computed(() => panel.program)
watch(() => panel.program?.revision, () => { selected.value = null })
const textCurrent = computed(() => program.value !== null && status.value !== null && program.value.file === status.value.program.file && program.value.revision === status.value.program.revision)
const previewCurrent = computed(() => panel.preview !== null && status.value !== null && panel.preview.context === status.value.preview_context)
const failure = computed(() => panel.preview?.state === 'failed' && previewCurrent.value ? panel.preview.error : null)
const unavailableReason = computed(() => panel.preview?.state === 'unavailable' && previewCurrent.value && panel.preview.error
  ? translateMessage(panel.preview.error.text) : undefined)
const drawn = useDrawnPreview(panel)
const failureLine = computed(() => textCurrent.value ? lineInText(failure.value?.line ?? 0) : 0)
const layouts = [['path', 'program.tab.layoutPreview'], ['text', 'program.tab.layoutCode'], ['split', 'program.tab.layoutSplit']] as const satisfies readonly (readonly [ProgramLayout, string])[]
// These are tool sections reported by the interpreter, not inferred CAM operations.
const sections = computed(() => {
  const rows: { tool: number; first: number; last: number; start: number }[] = []
  for (const [index, segment] of (drawn.value?.segments ?? []).entries()) {
    const last = rows.at(-1)
    if (last?.tool === segment.tool) last.last = Math.max(last.last, segment.line)
    else rows.push({ tool: segment.tool, first: segment.line, last: segment.line, start: index })
  }
  return rows
})
const spindleTool = computed(() => status.value?.tool.in_spindle ?? 0)
const selectedSection = computed(() => selected.value ?? sections.value.findIndex(section => section.tool === spindleTool.value))
const liveTelemetry = computed(() => live.value && status.value?.link === 'live' ? status.value : null)
const spindleText = computed(() => {
  const s = liveTelemetry.value
  if (!s) return ''
  const { rpm, atSpeed } = machineFeedback(s, panel.link)
  if (s.spindle.direction === 'stopped')
    return rpm.known && (s.spindle.actual_rpm ?? 0) >= 1 ? t('program.tab.spindle', { speed: rpm.text }) : ''
  const speed = rpm.known ? rpm.text : t('program.tab.commanded', { rpm: String(Math.round(s.spindle.commanded_rpm * s.spindle.override)) })
  if (!atSpeed.known) return t('program.tab.spindle', { speed })
  return s.spindle.at_speed ? t('program.tab.spindleAtSpeed', { speed }) : t('program.tab.spindleBelowSpeed', { speed })
})
const feedText = computed(() => {
  const s = liveTelemetry.value
  if (!s || (!programExecuting.value && s.motion.velocity === 0)) return ''
  return t('program.tab.feed', { feed: `${formatVelocity(s.motion.velocity, panel.unit)} ${velocityUnit(panel.unit)}` })
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
  const line = activeLine.value
  if (Number.isSafeInteger(line) && line > 0) {
    // Do not infer full-file progress from a partial prefix or an out-of-range counter.
    return !program.value.truncated && line <= count ? t('program.tab.lineOf', { line, count }) : t('program.tab.line', { line })
  }
  return program.value.truncated ? t('program.tab.linesShown', count) : t('program.tab.lines', count)
})
const activeLine = computed(() => {
  if (!programExecuting.value) return 0
  const breakpoint = status.value?.program.breakpoint
  return breakpoint?.reached ? breakpoint.line : currentLine.value || motionLine.value
})
const runState = computed(() => {
  const s = status.value
  if (!s || !live.value || s.machine !== 'on' || s.axes.some(axis => axis.homing)) return ''
  if (programExecuting.value && s.wait.kind === 'tool_change') return t('program.tab.changeTool', { tool: toolLabel(s.wait.tool) })
  if (programExecuting.value && s.wait.kind === 'paused')
    return activeLine.value ? t('program.tab.pausedAt', { line: activeLine.value }) : t('program.tab.paused')
  const tool = spindleTool.value > 0 ? toolLabel(spindleTool.value) : ''
  if (programExecuting.value) return tool ? t('program.tab.runningTool', { tool }) : t('program.tab.running')
  if (!s.motion.in_position) return t('program.tab.moving')
  if (s.interp !== 'idle') return ''
  return tool ? t('program.tab.idleTool', { tool }) : t('program.tab.idle')
})
// Cycle time from the drawn preview: before the run the estimate, during it elapsed and left.
const time = computed(() => {
  const preview = drawn.value
  const limits = velocityLimits(panel.hello)
  return preview !== null && limits !== null && preview.state === 'ready' ? cycleTime(preview, limits) : null
})
const sectionTimes = computed(() => time.value === null ? [] : sectionSeconds(time.value, sections.value))
const progressLine = computed(() => programExecuting.value ? motionLine.value || currentLine.value : 0)
// Where the run is in the timeline, followed forward: a subroutine or loop repeats its lines.
const progress = ref(-1)
watch([time, progressLine], ([estimate, line], [before]) => {
  progress.value = estimate === null || line === 0 ? -1 : progressAt(estimate, line, estimate === before ? progress.value : 0)
}, { immediate: true, flush: 'sync' })
const timeLabel = computed(() => {
  const estimate = time.value
  const elapsed = elapsedSeconds.value
  if (elapsed !== null && programExecuting.value) {
    // Feeds scale with the override; rapids do not, so this stays an estimate.
    const override = Math.max(status.value?.motion.feed_override ?? 1, 0.05)
    const left = estimate !== null && progressLine.value > 0 ? remainingFrom(estimate, progress.value) / override : null
    return left === null ? t('program.tab.elapsed', { elapsed: formatDuration(elapsed) })
      : t('program.tab.elapsedLeft', { elapsed: formatDuration(elapsed), left: formatDuration(left) })
  }
  if (estimate === null) return ''
  const changes = estimate.toolChanges
  return changes ? t('program.tab.estimateChanges', { time: formatDuration(estimate.total), changes: t('program.tab.toolChanges', changes) })
    : t('program.tab.estimate', { time: formatDuration(estimate.total) })
})
const timeTooltip = computed(() => {
  if (!timeLabel.value) return undefined
  if (runClock.lastSeconds === null) return timeLabel.value
  return `${timeLabel.value} · ${t('program.tab.lastRun', { time: formatDuration(runClock.lastSeconds), parts: t('program.tab.partCount', runClock.parts) })}`
})

// A restart is planned from an idle, loaded program whose text is the one the machine has.
const canRestart = computed(() => !props.readOnly && textCurrent.value && !programExecuting.value && status.value?.interp === 'idle' &&
  sections.value.length > 0 && !status.value.program.breakpoint)
const activeBreakpoint = computed(() => status.value?.program.breakpoint)
const breakpointLine = computed(() => activeBreakpoint.value?.line ?? selectedBreakpoint(status.value)?.line ?? 0)
const breakpointBlock = computed(() => machineBlock(panel, true) ??
  (homingRequired.value && !allHomed.value ? t('program.tab.notHomed') : null) ??
  (status.value?.interp !== 'idle' || !status.value.motion.in_position ? t('program.tab.stopToRun') : null))
const breakpointLines = computed(() => canRestart.value ? checkedBreakpointLines(checkedPreview.value) : new Set<number>())
const breakpointLabel = computed(() => activeBreakpoint.value?.reached
  ? t('program.tab.pausedBefore', { line: breakpointLine.value }) : t('program.tab.pauseBefore', { line: breakpointLine.value }))
function chooseBreakpoint(line: number): void {
  const loaded = status.value?.program
  const text = program.value?.lines[line - 1]
  if (breakpointBlock.value === null && loaded?.file && text !== undefined && breakpointLines.value.has(line))
    breakpointSelection.value = { file: loaded.file, revision: loaded.revision, line, text }
}
const restartAt = ref<{ line: number; section: string } | null>(null)
function sectionName(index: number): string {
  const section = sections.value[index]
  return section === undefined ? '' : t('program.tab.section', { n: index + 1, tool: toolLabel(section.tool) })
}
function restartLine(line: number): void {
  breakpointSelection.value = null
  let index = -1
  sections.value.forEach((section, candidate) => { if (section.first <= line) index = candidate })
  restartAt.value = { line, section: index < 0 ? t('program.tab.beforeFirst') : sectionName(index) }
}
// Where the program stopped before its end; offered only while a restart is possible.
// The operator can dismiss it; a stop at another line, or in another load, shows again.
const dismissedStop = ref('')
const stopKey = computed(() => {
  const program = status.value?.program
  const hint = program?.restart_hint
  return program && hint ? `${program.file ?? ''}:${String(program.revision)}:${String(hint.line)}` : ''
})
const stoppedAt = computed(() => canRestart.value && stopKey.value !== dismissedStop.value ? status.value?.program.restart_hint ?? null : null)
// The stopped-line sentence around its line number, which is shown in bold.
const stoppedText = computed((): [string, string] => {
  const hint = stoppedAt.value
  if (hint === null) return ['', '']
  const [before = '', after = ''] = t('program.tab.stoppedAt', { line: '\u0000', tool: toolLabel(hint.tool) }).split('\u0000')
  return [before, after]
})
function restartSection(index: number): void {
  const section = sections.value[index]
  if (section === undefined) return
  breakpointSelection.value = null
  restartAt.value = { line: section.first, section: sectionName(index) }
}
// Changing one feed or speed number of the loaded program; the machine saves and reopens it.
const editing = ref<{ line: number; text: string; word: RateWord; error: string } | null>(null)
let editRequest: CommandBody | null = null
function editRate(line: number, offset: number): void {
  const text = program.value?.lines[line - 1]
  const word = text === undefined ? undefined : rateWords(text).find((candidate) => candidate.end > offset)
  if (text !== undefined && word !== undefined) editing.value = { line, text, word, error: '' }
}
const rateUnit = computed(() => {
  if (editing.value?.word.letter === 'S') return 'rpm'
  return status.value?.program_units === 'inch' ? 'in/min' : 'mm/min'
})
function saveRate(value: number): void {
  const edit = editing.value
  const file = status.value?.program.file
  if (edit === null || !file || editRequest !== null) return
  const after = withRate(edit.text, edit.word, value)
  if (after === null) {
    edit.error = t('program.tab.badNumber')
    return
  }
  editRequest = { type: 'edit_program', path: file, line: edit.line, old_text: edit.text, new_text: after }
  send(editRequest)
}
const removeEditListener = onResult((command, result) => {
  if (command !== editRequest) return
  editRequest = null
  if (result.outcome === 'accepted') editing.value = null
  else if (editing.value !== null) editing.value.error = result.reason ? translateMessage(result.reason) : t('program.tab.notSaved')
})
onBeforeUnmount(removeEditListener)

// The path view and the text select together. In Split the text holds the selection; in Preview
// a tapped move shows its line in a chip, which opens the code at that line.
const textSelection = ref<{ first: number; last: number } | null>(null)
const pickedLine = ref(0)
watch([(): string => panel.programLayout, (): number | undefined => panel.program?.revision], () => {
  pickedLine.value = 0
  textSelection.value = null
})
function textSelected(first: number, last: number): void {
  textSelection.value = first > 0 ? { first, last } : null
}
const highlight = computed(() => panel.programLayout === 'path'
  ? pickedLine.value > 0 ? { first: pickedLine.value, last: pickedLine.value } : null
  : panel.programLayout === 'split' ? textSelection.value : null)
const pickedText = computed(() => textCurrent.value ? program.value?.lines[lineInText(pickedLine.value) - 1] ?? null : null)
function pathPicked(line: number): void {
  if (panel.programLayout === 'split') text.value?.select(textCurrent.value ? lineInText(line) : 0)
  else pickedLine.value = line
}
async function openPickedLine(): Promise<void> {
  const line = lineInText(pickedLine.value)
  if (line === 0) return
  panel.programLayout = 'split'
  await nextTick()
  text.value?.select(line)
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
          <span class="scene-title"><AppIcon :name="panel.programLayout === 'text' ? 'file' : 'fit'" />{{ panel.programLayout === 'text' ? t('program.tab.ncProgram') : t('program.tab.toolpathPreview') }}</span>
          <div class="scene-actions">
            <div class="view-switch" :aria-label="t('program.tab.viewLabel')"><button v-for="[layout, label] in layouts" :key="layout" class="tone-control choice" type="button" :aria-pressed="panel.programLayout === layout" @click="chooseProgramLayout(layout)">{{ t(label) }}</button></div>
          </div>
        </header>
        <div v-if="textCurrent && program?.truncated" class="failure" role="status"><AppIcon name="warning" /><span>{{ t('program.tab.partial', { count: program.lines.length }) }}</span></div>
        <div class="views" :class="panel.programLayout">
          <template v-if="panel.programLayout !== 'path'">
            <NcText
              v-if="program !== null && textCurrent" ref="text" :lines="program.lines" :motion-line="lineInText(motionLine)" :current-line="lineInText(currentLine)" :error-line="failureLine" :restartable="canRestart && !lineEdits.pending.value" :edits="readOnly ? undefined : lineEditBar"
              :breakpoint-line="breakpointLine" :breakpoint-lines="breakpointLines" :breakpoint-reached="activeBreakpoint?.reached === true"
              :breakpoint-block="breakpointBlock"
              @restart="restartLine" @edit-rate="editRate" @breakpoint="chooseBreakpoint"
              @line-edit="lineEdits.apply" @undo-line-edit="lineEdits.undoLast" @dismiss-line-edit="lineEdits.dismiss"
              @selection="textSelected"
            />
            <div v-else class="empty">{{ status?.program.file ? t('program.tab.waitingText') : t('program.tab.openToBegin') }}</div>
          </template>
          <PathView v-if="panel.programLayout !== 'text'" :highlight="highlight" @pick="pathPicked">
            <PartModel v-if="!readOnly" />
          </PathView>
          <div v-if="panel.programLayout === 'path' && pickedText !== null" class="picked-line">
            <button class="btn picked-open" type="button" :aria-label="t('program.tab.showLine', { line: pickedLine })" @click="openPickedLine">
              <strong class="mono">{{ t('program.tab.line', { line: pickedLine }) }}</strong><span class="mono picked-code">{{ pickedText }}</span>
            </button>
            <button class="btn picked-close" type="button" :aria-label="t('program.ncText.clearSelected')" @click="pickedLine = 0"><AppIcon name="close" /></button>
          </div>
          <div v-if="failure && (panel.programLayout === 'text' || failureLine > 0)" class="failure preview-diagnostic tinted-band" role="status">
            <AppIcon name="warning" /><span>{{ t('program.tab.previewStopped', { reason: translateMessage(failure.text) }) }}</span>
            <button v-if="failureLine > 0" class="btn show-line" type="button" @click="showFailure">{{ t('program.tab.showLine', { line: failureLine }) }}</button>
          </div>
          <SetterStatusBand v-else-if="unavailableReason && panel.programLayout === 'text'" class="preview-diagnostic" :line="{ text: unavailableReason, level: 'blocked' }" />
          <div v-if="stoppedAt" class="stopped-at" role="status">
            <button class="btn dismiss-stop" type="button" :aria-label="t('program.tab.dismissStop')" @click="dismissedStop = stopKey"><AppIcon name="close" /></button>
            <span>{{ stoppedText[0] }}<strong class="mono">{{ stoppedAt.line }}</strong>{{ stoppedText[1] }}</span>
            <button class="btn primary" type="button" @click="restartLine(stoppedAt.line)">{{ t('program.tab.restartThere') }}</button>
          </div>
        </div>
      </section>
      <aside class="inspector">
        <div class="inspector-body">
          <div class="program-actions">
            <button
              v-if="status?.program.file" v-tooltip="checkLabel" type="button" class="btn program-check"
              :aria-label="t('program.tab.checkLabel', { state: checkLabel })"
              :class="{ blocked: check?.errors.length, review: check?.warnings.length, ready: check && !check.errors.length && !check.warnings.length }"
              @click="checkOpen = true"
            >
              <AppIcon :name="check?.errors.length || check?.warnings.length ? 'warning' : check ? 'check' : 'list'" />
              {{ t('program.tab.check') }}<span v-if="check?.warnings.length && !check.errors.length">{{ check.warnings.length }}</span>
            </button>
            <button
              v-if="breakpointLine && !readOnly" class="btn breakpoint-chip" type="button" :disabled="!!activeBreakpoint"
              :aria-label="activeBreakpoint ? breakpointLabel : t('program.tab.clearBreakpoint')" @click="breakpointSelection = null"
            >
              {{ breakpointLabel }}<AppIcon v-if="!activeBreakpoint" name="close" />
            </button>
          </div>
          <div class="panel-head"><h2>{{ t('program.tab.toolSequence') }}</h2></div>
          <div class="sections">
            <div v-for="(section, index) in sections" :key="`${section.first}-${section.tool}`" class="section-row">
              <button class="section list-row" type="button" :aria-pressed="selectedSection === index" @click="selected = index">
                <span class="section-number mono">{{ index + 1 }}</span>
                <span class="section-text"><strong>{{ section.tool === 0 ? t('program.tab.noTool') : findTool(section.tool)?.comment || t('program.tab.toolN', { n: section.tool }) }}</strong><small>T{{ section.tool }} · {{ t('program.tab.linesRange', { first: section.first, last: section.last }) }}<template v-if="sectionTimes[index] !== undefined"> · {{ formatDuration(sectionTimes[index] ?? 0) }}</template></small></span>
              </button>
              <button v-if="canRestart && selectedSection === index" class="btn section-restart" type="button" :aria-label="t('program.tab.restartSection', { section: sectionName(index) })" @click="restartSection(index)">{{ t('program.tab.restart') }}</button>
            </div>
            <p v-if="sections.length === 0" class="quiet">{{ previewCurrent ? t('program.tab.noSequence') : t('program.tab.sequenceUnavailable') }}</p>
          </div>
        </div>
      </aside>
    </div>
    <NumberDialog
      v-if="editing" :key="`${String(editing.line)}-${String(editing.word.begin)}`"
      :title="editing.word.letter === 'F' ? t('program.tab.editFeed', { line: editing.line }) : t('program.tab.editSpeed', { line: editing.line })"
      :effect="t('program.tab.editEffect')"
      :label="editing.word.letter === 'F' ? t('program.tab.feedLabel') : t('program.tab.speedLabel')" :unit="rateUnit" :initial="String(editing.word.value)"
      :action="t('program.tab.saveProgram')" :error="editing.error" @confirm="saveRate" @close="editing = null"
    />
    <RestartDialog v-if="restartAt" :line="restartAt.line" :section="restartAt.section" @close="restartAt = null" />
    <ProgramCheckDialog v-if="checkOpen" :check="check" :unit="panel.unit" :max-rpm="panel.hello?.max_spindle_rpm" @close="checkOpen = false" />
    <div class="status-row">
      <span>{{ runState }}</span>
      <span v-if="spindleText" class="spindle mono">{{ spindleText }}</span>
      <span v-if="feedText" class="feed mono">{{ feedText }}</span>
      <span v-tooltip="timeTooltip" class="run-time mono">{{ timeLabel }}</span>
      <span class="line-count mono">{{ lineLabel }}</span>
    </div>
  </div>
</template>

<style scoped>
.program { display: flex; flex-direction: column; height: 100%; min-height: 0; gap: 14px; }
.workspace-card { display: grid; grid-template-columns: minmax(0, 1fr) 292px; flex: 1; min-height: 0; border-radius: var(--radius); background: var(--panel); overflow: hidden; }
.scene { display: flex; flex-direction: column; min-width: 0; min-height: 0; background: var(--bg-sunken); }
.scene-top { display: flex; flex-wrap: wrap; align-items: center; justify-content: space-between; padding: 6px 12px 6px 18px; gap: 4px 12px; flex: none; }
.scene-title { display: flex; align-items: center; gap: 8px; font-size: var(--type-body); font-weight: 600; }.scene-title :deep(svg) { width: 16px; }
.scene-actions { display: flex; align-items: center; gap: 9px; }
.program-check, .breakpoint-chip { min-height: var(--touch); padding: 5px 10px; white-space: nowrap; font-size: var(--type-sm); }
.breakpoint-chip, .breakpoint-chip:disabled { color: var(--warn); background: var(--warn-soft); opacity: 1; }
.breakpoint-chip :deep(.icon) { width: 13px; height: 13px; }
.program-check :deep(.icon) { width: 14px; height: 14px; }
.program-actions { display: flex; flex-wrap: wrap; gap: 6px; margin-bottom: 12px; }
.program-check.ready { color: var(--go); background: var(--go-soft); }
.program-check.blocked { color: var(--stop); background: var(--stop-soft); }
.program-check.review:not(.blocked) { color: var(--warn); background: var(--warn-soft); }
.view-switch { display: flex; align-items: center; gap: 3px; }
.view-switch button { min-width: var(--touch); min-height: var(--touch); border: 0; border-radius: var(--control-radius); padding: 0 12px; font-size: var(--type-body); font-weight: 600; color: var(--text-dim); cursor: pointer; }
.view-switch button:hover { background: var(--control-hover); }
.view-switch button:active { background: var(--control-active); }
.views { position: relative; flex: 1; display: grid; min-height: 0; min-width: 0; }.views.split { grid-template-columns: 1fr 1fr; }.views.text { padding: 8px 16px 16px; }
.views :deep(.nctext) { border: 0; border-radius: 0; }.empty { display: grid; place-items: center; color: var(--text-dim); }
.inspector { display: flex; flex-direction: column; min-height: 0; }
.panel-head { margin-bottom: 12px; }h2 { margin: 0; font-size: var(--type-md); font-weight: 600; letter-spacing: -.15px; }
.sections { display: flex; flex-direction: column; gap: 5px; margin: 0 -8px 14px; }
.section-row { display: flex; align-items: center; gap: 6px; }.section-row .section { flex: 1; min-width: 0; }.section-restart { flex: none; min-height: var(--touch); padding: 0 10px; font-size: var(--type-sm); }
.section { display: flex; align-items: center; gap: 10px; min-height: 57px; padding: 10px 8px; text-align: left; }
.section[aria-pressed=true] { background: var(--accent-soft); }.section-number { display: grid; place-items: center; width: 26px; height: 26px; border-radius: 50%; background: var(--bg-sunken); color: var(--text-dim); font-size: var(--type-sm); flex: none; }
.section[aria-pressed=true] .section-number { background: var(--panel); color: var(--accent); }.section-text { display: flex; flex-direction: column; gap: 3px; }.section-text strong { font-weight: 600; font-size: var(--type-body); }.section-text small { font-size: var(--type-sm); color: var(--text-dim); }
.context-note, .quiet { margin: 15px 0 0; font-size: var(--type-sm); color: var(--text-dim); }
.status-row { flex: none; min-height: 20px; display: flex; justify-content: flex-end; align-items: center; gap: 20px; font-size: 12px; }
.status-row > span { min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.status-row > .mono { font-size: var(--type-sm); color: var(--text-dim); }
.line-count { text-align: right; }
.run-time { text-align: center; }
.failure { display: flex; align-items: center; gap: 8px; padding: 8px 18px; color: var(--stop); font-size: 12px; }.failure .show-line { flex: none; min-height: 36px; padding-inline: 10px; font-size: var(--type-sm); }.failure span { flex: 1; min-width: 0; overflow-wrap: anywhere; }
.preview-diagnostic { position: absolute; z-index: 1; inset: 8px 16px auto; margin: 0; pointer-events: none; }
.failure.preview-diagnostic { border-radius: 9px; background: var(--stop-soft); }
.preview-diagnostic button { pointer-events: auto; }
/* The stop hint shares the bottom of the view with transient code actions. */
.stopped-at { position: absolute; z-index: 1; left: 16px; bottom: 12px; display: flex; align-items: center; gap: 12px; padding: 6px; border-radius: 10px; background: var(--panel-raised); font-size: 13px; box-shadow: 0 4px 14px rgb(0 0 0 / 12%); }
.views:has(.line-actions, .follow) .stopped-at { display: none; }
/* A tapped move's line, over the top right of the preview, where nothing else is drawn. */
.picked-line { position: absolute; z-index: 1; top: 10px; right: 16px; display: flex; gap: 4px; max-width: min(60%, 520px); padding: 4px; border-radius: 10px; background: var(--panel-raised); box-shadow: 0 4px 14px rgb(0 0 0 / 12%); }
.picked-line .btn { height: 36px; }
.picked-open { display: flex; align-items: center; gap: 10px; min-width: 0; padding: 0 10px; font-size: 12px; }
.picked-open strong { flex: none; font-weight: 600; color: var(--accent); }
.picked-code { min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: pre; color: var(--text); }
.picked-close { flex: none; min-width: 36px; padding: 0; }
.stopped-at .btn { height: 36px; }.dismiss-stop { min-width: 36px; padding: 0; }
@media (max-height: 850px) { .program { gap: 10px; }.section { min-height: 50px; }.context-note { margin-top: 10px; } }
.inspector { overflow: hidden; }
.inspector-body { display: flex; flex-direction: column; flex: 1; min-height: 0; padding: 20px; overflow-y: auto; }
.inspector-body > * { flex-shrink: 0; }
@media (max-height: 850px) { .inspector-body { padding: 14px 20px; } }
@media (max-height: 850px) {
  .inspector-body { padding-block: 10px; }
  .panel-head { margin-bottom: 6px; }
  .sections { gap: 0; margin-bottom: 8px; }
  .section { min-height: 44px; padding-block: 6px; }
}
</style>
