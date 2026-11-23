<script setup lang="ts">
import { computed, onBeforeUnmount, ref, shallowRef } from 'vue'
import LogDialog from './components/LogDialog.vue'
import DroPanel from './components/DroPanel.vue'
import ManualTab from './components/ManualTab.vue'
import ProgramTab from './components/ProgramTab.vue'
import RunBar from './components/RunBar.vue'
import StatusBar from './components/StatusBar.vue'
import ToolChangePrompt from './components/ToolChangePrompt.vue'
import RestartDialog from './components/RestartDialog.vue'
import ToolsTab from './components/ToolsTab.vue'
import FileBrowser from './components/FileBrowser.vue'
import OperationDialog from './components/OperationDialog.vue'
import ProgramEditor from './components/ProgramEditor.vue'
import NumericEntryDialog from './components/NumericEntryDialog.vue'
import RemoteScreen from './components/RemoteScreen.vue'
import { isRemoteScreen } from './bridge'
import SettingsDialog from './components/SettingsDialog.vue'
import QuitDialog from './components/QuitDialog.vue'
import DiagnosticsDialog from './components/DiagnosticsDialog.vue'
import { startDialog } from './dialogs'
import { editorTarget } from './program-editor'
import { onResult, operatorSettings, panel, send } from './machine'
import type { CommandBody, RestartPlan } from './protocol'

const logOpen = ref<boolean>(startDialog === 'log')
const browsing = ref<boolean>(startDialog === 'open')
const creating = ref<boolean>(startDialog === 'operation')
// The one Edit program action: the loaded program, or a new one when none is loaded.
function edit(): void {
  editorTarget.value = panel.status?.program.file ? { kind: 'loaded' } : { kind: 'new', folder: '', taken: [] }
}
// Development: the editor waits for the scene's program text.
if (startDialog === 'edit') editorTarget.value = { kind: 'loaded' }
// New program, from inside the editor: a fresh editor in the same folder.
const editorKey = ref(0)
function newProgram(folder: string): void {
  editorKey.value += 1
  editorTarget.value = { kind: 'new', folder, taken: [] }
}
const quitOpen = ref(false)
const settingsOpen = ref(false)
const diagnosticsOpen = ref<boolean>(startDialog === 'diagnostics')
const toolChange = computed(() => panel.status?.wait.kind === 'tool_change' ? panel.status.wait : null)
const measurement = shallowRef<{ tool: number; command: CommandBody } | null>(null)
const measuredRestart = shallowRef<RestartPlan | null>(null)
const promptTool = computed(() => measurement.value?.tool ?? toolChange.value?.tool ?? null)

function measureTool(): void {
  const wait = toolChange.value
  if (measurement.value || wait?.measure_request === undefined) return
  const command: CommandBody = { type: 'tool_changed', measure_request: wait.measure_request }
  measurement.value = { tool: wait.tool, command }
  send(command)
}

onBeforeUnmount(onResult((command, result) => {
  if (command !== measurement.value?.command) return
  measurement.value = null
  if (result.outcome === 'timed_out') send({ type: 'abort' })
  if (result.outcome === 'accepted' && result.restart) measuredRestart.value = result.restart
}))
</script>

<template>
  <RemoteScreen v-if="isRemoteScreen()" />
  <div v-else class="app">
    <StatusBar @log="logOpen = true" @open="browsing = true" @create="creating = true" @edit="edit" @quit="quitOpen = true" @settings="settingsOpen = true" @diagnostics="diagnosticsOpen = true" />
    <div class="workspace">
      <DroPanel />
      <main class="middle">
        <ManualTab v-if="panel.tab === 'manual'" />
        <ProgramTab v-else-if="panel.tab === 'program'" />
        <ManualTab v-else-if="panel.tab === 'probe'" touch-off />
        <ToolsTab v-else />
        <ToolChangePrompt
          v-if="promptTool !== null" :tool="promptTool" :measure="toolChange?.measure_request !== undefined"
          :measuring="measurement !== null" :class="{ 'with-status-row': panel.tab === 'program' }"
          @measure="measureTool"
        />
        <RestartDialog
          v-if="measuredRestart" :line="measuredRestart.line" section="" :prepared="measuredRestart"
          @close="measuredRestart = null"
        />
        <div id="dialogs"></div>
        <LogDialog v-if="logOpen" @close="logOpen = false" />
        <FileBrowser v-if="browsing" @close="browsing = false" />
        <OperationDialog v-if="creating" @close="creating = false" />
        <ProgramEditor v-if="editorTarget" :key="editorKey" :target="editorTarget" @close="editorTarget = null" @new-program="newProgram" />
        <SettingsDialog v-if="settingsOpen" :settings="operatorSettings" @close="settingsOpen = false" />
        <QuitDialog v-if="quitOpen" @close="quitOpen = false" />
        <DiagnosticsDialog v-if="diagnosticsOpen" @close="diagnosticsOpen = false" />
      </main>
      <div id="workspace-dialogs"></div>
      <NumericEntryDialog />
    </div>
    <RunBar />
  </div>
</template>

<style scoped>
.app { display: flex; flex-direction: column; width: 100%; height: 100%; }
.workspace { position: relative; display: flex; flex-direction: column; flex: 1; min-height: 0; }
.middle { position: relative; flex: 1; min-height: 0; margin: 0 24px 14px; }
#dialogs, #workspace-dialogs { position: absolute; inset: 0; z-index: 20; pointer-events: none; }
@media (max-height: 760px) { .middle { margin-bottom: 10px; } }
</style>
