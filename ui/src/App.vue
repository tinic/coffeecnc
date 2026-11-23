<script setup lang="ts">
import { computed, ref } from 'vue'
import LogDialog from './components/LogDialog.vue'
import DroPanel from './components/DroPanel.vue'
import ManualTab from './components/ManualTab.vue'
import ProgramTab from './components/ProgramTab.vue'
import RunBar from './components/RunBar.vue'
import StatusBar from './components/StatusBar.vue'
import ToolChangePrompt from './components/ToolChangePrompt.vue'
import ToolsTab from './components/ToolsTab.vue'
import FileBrowser from './components/FileBrowser.vue'
import OperationDialog from './components/OperationDialog.vue'
import ProgramEditor from './components/ProgramEditor.vue'
import NumericEntryDialog from './components/NumericEntryDialog.vue'
import SettingsDialog from './components/SettingsDialog.vue'
import QuitDialog from './components/QuitDialog.vue'
import { startDialog } from './dialogs'
import { editorTarget } from './program-editor'
import { operatorSettings, panel } from './machine'

const logOpen = ref<boolean>(startDialog === 'log')
const browsing = ref<boolean>(startDialog === 'open')
const creating = ref<boolean>(startDialog === 'operation')
if (startDialog === 'edit') editorTarget.value = { kind: 'loaded' }
const quitOpen = ref(false)
const settingsOpen = ref(false)
const toolChange = computed(() => panel.status?.wait.kind === 'tool_change' ? panel.status.wait.tool : null)
</script>

<template>
  <div class="app">
    <StatusBar @log="logOpen = true" @open="browsing = true" @create="creating = true" @edit="editorTarget = { kind: 'loaded' }" @quit="quitOpen = true" @settings="settingsOpen = true" />
    <div class="workspace">
      <DroPanel />
      <main class="middle">
        <ManualTab v-if="panel.tab === 'manual'" />
        <ProgramTab v-else-if="panel.tab === 'program'" />
        <ManualTab v-else-if="panel.tab === 'probe'" touch-off />
        <ToolsTab v-else />
        <ToolChangePrompt v-if="toolChange !== null" :tool="toolChange" :class="{ 'with-status-row': panel.tab === 'program' }" />
        <div id="dialogs"></div>
        <LogDialog v-if="logOpen" @close="logOpen = false" />
        <FileBrowser v-if="browsing" @close="browsing = false" @new-program="(folder, taken) => { browsing = false; editorTarget = { kind: 'new', folder, taken } }" />
        <OperationDialog v-if="creating" @close="creating = false" />
        <ProgramEditor v-if="editorTarget" :target="editorTarget" @close="editorTarget = null" />
        <SettingsDialog v-if="settingsOpen" :settings="operatorSettings" @close="settingsOpen = false" />
        <QuitDialog v-if="quitOpen" @close="quitOpen = false" />
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
