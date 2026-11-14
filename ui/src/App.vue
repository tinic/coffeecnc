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
import { startDialog } from './dialogs'
import { panel } from './machine'

const logOpen = ref<boolean>(startDialog === 'log')
const browsing = ref<boolean>(startDialog === 'open')
const toolChange = computed(() => panel.status?.wait.kind === 'tool_change' ? panel.status.wait.tool : null)
</script>

<template>
  <div class="app">
    <StatusBar @log="logOpen = true" @open="browsing = true" />
    <DroPanel />
    <main class="middle">
      <ManualTab v-if="panel.tab === 'manual'" />
      <ProgramTab v-else-if="panel.tab === 'program'" />
      <ToolsTab v-else />
      <ToolChangePrompt v-if="toolChange !== null" :tool="toolChange" :class="{ 'with-status-row': panel.tab === 'program' }" />
      <div id="dialogs"></div>
      <LogDialog v-if="logOpen" @close="logOpen = false" />
      <FileBrowser v-if="browsing" @close="browsing = false" />
    </main>
    <RunBar />
    <footer class="foot"><span>{{ panel.simulated ? 'SIMULATOR · No controller connected' : 'CoffeeCNC · Local machine interface' }}</span><span>coffeecnc / mill</span></footer>
  </div>
</template>

<style scoped>
.app { display: flex; flex-direction: column; width: 100%; height: 100%; }
.middle { position: relative; flex: 1; min-height: 0; margin: 0 24px 14px; }
#dialogs { position: absolute; inset: 0; z-index: 20; pointer-events: none; }
.foot { display: flex; justify-content: space-between; padding: 9px 24px; color: var(--text-dim); font-size: 10px; letter-spacing: .2px; }
@media (max-height: 760px) { .middle { margin-bottom: 10px; } .foot { padding-block: 5px; } }
</style>
