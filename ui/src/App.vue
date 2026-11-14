<script setup lang="ts">
// Layout: status bar on top and run bar at the bottom, always; between them the position readout
// and one workspace. Switching workspace is a change of view only and sends nothing.
import { computed, ref } from 'vue'

import LogDialog from './components/LogDialog.vue'
import DroPanel from './components/DroPanel.vue'
import ManualTab from './components/ManualTab.vue'
import ProgramTab from './components/ProgramTab.vue'
import RunBar from './components/RunBar.vue'
import StatusBar from './components/StatusBar.vue'
import ToolChangePrompt from './components/ToolChangePrompt.vue'
import ToolsTab from './components/ToolsTab.vue'
import { startDialog } from './dialogs'
import { panel } from './machine'
import type { Tab } from './store'

const logOpen = ref<boolean>(startDialog === 'log')

const tabNames: readonly (readonly [Tab, string])[] = [
  ['manual', 'Manual'],
  ['program', 'Program'],
  ['tools', 'Tools & offsets'],
]

const toolChange = computed(() => (panel.status?.wait.kind === 'tool_change' ? panel.status.wait.tool : null))
</script>

<template>
  <div class="app">
    <StatusBar @log="logOpen = true" />
    <main class="middle">
      <DroPanel />
      <section class="workspace">
        <nav class="tabs" aria-label="Workspace">
          <button
            v-for="[tab, name] in tabNames"
            :key="tab"
            class="tab"
            type="button"
            :class="{ on: panel.tab === tab }"
            :aria-current="panel.tab === tab ? 'page' : undefined"
            @click="panel.tab = tab"
          >
            {{ name }}
          </button>
        </nav>
        <div class="content">
          <ManualTab v-if="panel.tab === 'manual'" />
          <ProgramTab v-else-if="panel.tab === 'program'" />
          <ToolsTab v-else />
        </div>
      </section>
      <ToolChangePrompt v-if="toolChange !== null" :tool="toolChange" />
      <div id="dialogs"></div>
      <LogDialog v-if="logOpen" @close="logOpen = false" />
    </main>
    <RunBar />
  </div>
</template>

<style scoped>
.app {
  display: flex;
  flex-direction: column;
  width: 100%;
  height: 100%;
}

.middle {
  position: relative;
  display: flex;
  flex: 1;
  min-height: 0;
}

.workspace {
  display: flex;
  flex: 1;
  flex-direction: column;
  min-width: 0;
  min-height: 0;
}

.tabs {
  display: flex;
  flex: none;
  gap: 4px;
  padding: 8px 12px 0;
  border-bottom: 1px solid var(--line);
}

.tab {
  min-width: 132px;
  height: 44px;
  padding: 0 18px;
  border: 1px solid transparent;
  border-bottom: 0;
  border-radius: var(--radius) var(--radius) 0 0;
  background: none;
  font-weight: 600;
  color: var(--text-dim);
  cursor: pointer;
}

.tab:hover {
  color: var(--text);
}

.tab.on {
  margin-bottom: -1px;
  border-color: var(--line);
  background: var(--bg);
  box-shadow: inset 0 2px 0 var(--accent);
  color: var(--text);
}

.content {
  flex: 1;
  min-height: 0;
  padding: 10px 12px;
}

/* Dialogs cover the workspace only: the position readout stays in sight. */
#dialogs {
  position: absolute;
  inset: 0 0 0 376px;
  z-index: 20;
  pointer-events: none;
}
</style>
