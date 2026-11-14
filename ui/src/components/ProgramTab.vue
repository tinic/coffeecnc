<script setup lang="ts">
import { computed, nextTick, ref, useTemplateRef, watch } from 'vue'
import { findTool, live, panel, reported } from '../machine'
import type { ProgramLayout } from '../store'
import { formatLength, formatVelocity, velocityUnit } from '../units'
import AppIcon from './AppIcon.vue'
import NcText from './NcText.vue'
import PathView from './PathView.vue'

const status = reported
const text = useTemplateRef<InstanceType<typeof NcText>>('text')
const selected = ref<number | null>(null)
const program = computed(() => panel.program)
watch(() => panel.program?.revision, () => { selected.value = null })
const textCurrent = computed(() => program.value !== null && status.value !== null && program.value.revision === status.value.program.revision)
const previewCurrent = computed(() => panel.preview !== null && status.value !== null && panel.preview.context === status.value.preview_context)
const failure = computed(() => panel.preview?.state === 'failed' && previewCurrent.value ? panel.preview.error : null)
const layouts: readonly [ProgramLayout, string][] = [['path', 'Preview'], ['text', 'Code'], ['split', 'Split']]
// These are tool sections reported by the interpreter, not inferred CAM operations.
const sections = computed(() => {
  const rows: { tool: number; first: number; last: number }[] = []
  if (!previewCurrent.value) return rows
  for (const segment of panel.preview?.segments ?? []) {
    const last = rows.at(-1)
    if (last?.tool === segment.tool) last.last = Math.max(last.last, segment.line)
    else rows.push({ tool: segment.tool, first: segment.line, last: segment.line })
  }
  return rows
})
const currentTool = computed(() => selected.value ?? status.value?.tool.in_spindle ?? 0)
const tool = computed(() => findTool(currentTool.value))
const runState = computed(() => {
  const s = status.value
  if (!live.value) return 'Status unavailable · displayed values may be out of date'
  if (s?.wait.kind === 'tool_change') return `Waiting for tool ${String(s.wait.tool)}`
  if (s?.wait.kind === 'paused') return 'Program paused'
  if (s?.wait.kind === 'dwell') return 'Dwelling'
  if (s?.interp !== 'idle') return s?.mode === 'mdi' ? 'MDI command running' : 'Program running'
  return s.program.file ? 'Program loaded · machine idle' : 'No program loaded'
})
async function showFailure(): Promise<void> {
  panel.programLayout = 'text'
  await nextTick()
  if (failure.value) text.value?.goTo(failure.value.line)
}
</script>

<template>
  <div class="program">
    <div class="workspace-card">
      <section class="scene">
        <header class="scene-top">
          <span class="scene-title"><AppIcon :name="panel.programLayout === 'text' ? 'file' : 'fit'" />{{ panel.programLayout === 'text' ? 'NC program' : 'Toolpath preview' }}</span>
          <div class="view-switch" aria-label="Program view"><button v-for="[layout, label] in layouts" :key="layout" type="button" :aria-pressed="panel.programLayout === layout" @click="panel.programLayout = layout">{{ label }}</button></div>
        </header>
        <div v-if="failure" class="failure" role="status"><AppIcon name="warning" /><span>Preview stopped: {{ failure.text }}</span><button type="button" @click="showFailure">Show line {{ failure.line }}</button></div>
        <div class="views" :class="panel.programLayout">
          <template v-if="panel.programLayout !== 'path'">
            <NcText v-if="program !== null && textCurrent" ref="text" :lines="program.lines" :motion-line="live ? (status?.program.motion_line ?? 0) : 0" :current-line="live ? (status?.program.current_line ?? 0) : 0" :error-line="failure?.line ?? 0" />
            <div v-else class="empty">{{ status?.program.file ? 'Waiting for program text' : 'Open a program to begin' }}</div>
          </template>
          <PathView v-if="panel.programLayout !== 'text'" />
        </div>
      </section>
      <aside class="inspector">
        <div class="panel-head"><h2>Tool sequence</h2><span class="kicker">{{ sections.length }} sections</span></div>
        <div class="sections">
          <button v-for="(section, index) in sections" :key="`${section.first}-${section.tool}`" class="section" type="button" :aria-pressed="currentTool === section.tool" @click="selected = section.tool">
            <span class="section-number mono">{{ index + 1 }}</span>
            <span class="section-text"><strong>{{ section.tool === 0 ? 'No tool selected' : findTool(section.tool)?.comment || `Tool ${String(section.tool)}` }}</strong><small>T{{ section.tool }} · Lines {{ section.first }}–{{ section.last }}</small></span>
          </button>
          <p v-if="sections.length === 0" class="quiet">{{ previewCurrent ? 'No tool sequence in this preview.' : 'Tool sequence unavailable.' }}</p>
        </div>
        <div class="tool">
          <div class="kicker">{{ currentTool === status?.tool.in_spindle ? 'Tool in spindle' : 'Inspected tool' }}</div>
          <div class="tool-title"><span class="tool-glyph"><AppIcon name="target" /></span><div><strong>{{ currentTool === 0 ? 'No tool loaded' : `T${String(currentTool)} · ${tool?.comment || 'Tool not in table'}` }}</strong><small v-if="tool">Ø{{ formatLength(tool.diameter, panel.unit) }} {{ panel.unit }}</small></div></div>
          <div v-if="tool" class="kv"><span>Table Z offset</span><strong class="mono">{{ formatLength(tool.length, panel.unit) }} {{ panel.unit }}</strong></div>
          <div class="kv"><span>Spindle command</span><strong>{{ status?.spindle.direction === 'stopped' ? 'Stopped' : `${String(Math.round((status?.spindle.commanded_rpm ?? 0) * (status?.spindle.override ?? 1)))} rpm` }}</strong></div>
          <div class="kv"><span>Current feed</span><strong>{{ status ? formatVelocity(status.motion.velocity, panel.unit) : '—' }} {{ velocityUnit(panel.unit) }}</strong></div>
          <p class="context-note">Toolpath only · no part model associated</p>
        </div>
      </aside>
    </div>
    <div class="status-row"><span :class="{ warning: !live }">{{ runState }}</span><span class="mono">{{ status && status.program.current_line > 0 ? `Line ${String(status.program.current_line)}` : '—' }}<template v-if="textCurrent"> / {{ program?.lines.length }}</template></span></div>
  </div>
</template>

<style scoped>
.program { display: flex; flex-direction: column; height: 100%; min-height: 0; gap: 14px; }
.workspace-card { display: grid; grid-template-columns: minmax(0, 1fr) 292px; flex: 1; min-height: 0; border: 1px solid var(--line); border-radius: 12px; background: var(--panel); overflow: hidden; }
.scene { display: flex; flex-direction: column; min-width: 0; min-height: 0; background: var(--bg-sunken); }
.scene-top { display: flex; align-items: center; justify-content: space-between; padding: 12px 18px 4px; gap: 12px; flex: none; }
.scene-title { display: flex; align-items: center; gap: 8px; font-size: 12px; font-weight: 500; }.scene-title :deep(svg) { width: 16px; }
.view-switch { display: flex; gap: 2px; }.view-switch button { border: 0; border-radius: 7px; padding: 8px 10px; font-size: 11px; color: var(--text-dim); background: none; cursor: pointer; }.view-switch button[aria-pressed=true] { color: var(--accent); background: var(--panel); }
.views { flex: 1; display: grid; min-height: 0; min-width: 0; }.views.split { grid-template-columns: 1fr 1fr; }.views.text { padding: 8px 16px 16px; }
.views :deep(.nctext) { border: 0; border-radius: 0; }.empty { display: grid; place-items: center; color: var(--text-dim); }
.inspector { display: flex; flex-direction: column; min-height: 0; padding: 20px; border-left: 1px solid var(--line); overflow-y: auto; }
.panel-head { display: flex; align-items: baseline; justify-content: space-between; gap: 12px; margin-bottom: 12px; }h2 { margin: 0; font-size: 14px; font-weight: 600; letter-spacing: -.15px; }.kicker { font-size: 10px; letter-spacing: .8px; color: var(--text-dim); text-transform: uppercase; }
.sections { display: flex; flex-direction: column; gap: 5px; margin: 0 -8px 14px; }
.section { display: flex; align-items: center; gap: 10px; min-height: 57px; padding: 10px 8px; border: 0; border-radius: 9px; background: none; text-align: left; cursor: pointer; }
.section[aria-pressed=true] { background: var(--accent-soft); }.section-number { display: grid; place-items: center; width: 25px; height: 25px; border: 1px solid var(--line); border-radius: 50%; color: var(--text-dim); font-size: 11px; flex: none; }
.section[aria-pressed=true] .section-number { border-color: color-mix(in srgb, var(--accent) 40%, transparent); color: var(--accent); }.section-text { display: flex; flex-direction: column; gap: 4px; }.section-text strong { font-weight: 500; font-size: 13px; }.section-text small { font-size: 11px; color: var(--text-dim); }
.tool { border-top: 1px solid var(--line); padding-top: 16px; margin-top: auto; }.tool-title { display: flex; align-items: center; gap: 10px; margin: 12px 0 16px; }.tool-title strong { font-size: 13px; font-weight: 500; }.tool-title small { display: block; margin-top: 4px; color: var(--text-dim); font-size: 11px; }.tool-glyph { display: flex; padding: 10px; background: var(--bg-sunken); border-radius: 8px; color: var(--text-dim); }
.kv { display: flex; justify-content: space-between; gap: 8px; margin: 10px 0; font-size: 12px; color: var(--text-dim); }.kv strong { color: var(--text); font-weight: 500; }.kv .mono { font-size: 11px; }
.context-note, .quiet { margin: 15px 0 0; font-size: 11px; color: var(--text-dim); }
.status-row { display: flex; align-items: center; justify-content: space-between; gap: 12px; font-size: 12px; }.status-row .mono { font-size: 10px; color: var(--text-dim); }.warning { color: var(--stop); }
.failure { display: flex; align-items: center; gap: 8px; padding: 8px 18px; color: var(--stop); font-size: 12px; }.failure button { border: 0; background: none; color: inherit; text-decoration: underline; cursor: pointer; flex: none; }.failure span { flex: 1; }
@media (max-height: 850px) { .program { gap: 10px; }.inspector { padding: 14px 20px; }.section { min-height: 50px; }.tool-title { margin-block: 8px; }.kv { margin-block: 7px; }.context-note { margin-top: 10px; }.tool { padding-top: 12px; } }
</style>
