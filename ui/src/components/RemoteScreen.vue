<script setup lang="ts">
import { computed, ref } from 'vue'
import { useI18n } from 'vue-i18n'

import { theme } from '../appearance'
import { live, panel, reported } from '../machine'
import { isDarkTheme } from '../operator-settings'
import { translateMessage } from '../i18n/messages'
import { baseName } from '../units'
import DroPanel from './DroPanel.vue'
import LogDialog from './LogDialog.vue'
import ProgramTab from './ProgramTab.vue'

const { t } = useI18n()
const logOpen = ref(false)
const state = computed(() => {
  const status = reported.value
  if (!live.value || status === null) return t('status.remote.noLive')
  if (status.machine === 'estop') return t('status.remote.estop')
  if (status.machine === 'off') return t('status.remote.machineOff')
  if (status.interp === 'paused') return t('status.remote.paused')
  if (status.interp !== 'idle') return t('status.remote.running')
  return t('status.remote.machineOn')
})
const file = computed(() => reported.value?.program.file)
</script>

<template>
  <div class="remote">
    <header>
      <img :src="`./branding/coffeecnc-logo-${isDarkTheme(theme) ? 'dark' : 'light'}.svg`" alt="CoffeeCNC" width="161" height="36" />
      <span class="read-only">{{ t('status.remote.readOnly') }}</span>
      <strong v-if="panel.hello?.machine_name">{{ panel.hello.machine_name }}</strong>
      <span
        role="status" class="machine-state" :class="{
          stopped: live && reported?.machine === 'estop',
          stale: !live || reported?.interp === 'paused',
          off: live && reported?.machine === 'off',
        }"
      >{{ state }}</span>
      <button class="btn" type="button" @click="logOpen = true">{{ t('status.remote.messageLog') }}<span v-if="panel.log.length"> · {{ panel.log.length }}</span></button>
    </header>
    <!-- E-stop, errors and other notices show also with no program loaded. -->
    <section v-if="file || panel.notice" class="job">
      <h1 v-if="file">{{ baseName(file) }}</h1>
      <p v-if="panel.notice" class="notice" :class="{ error: panel.notice.level === 'error' }">{{ translateMessage(panel.notice.text) }}</p>
    </section>
    <DroPanel read-only />
    <main>
      <ProgramTab read-only />
      <div id="dialogs"></div>
      <div id="workspace-dialogs"></div>
      <LogDialog v-if="logOpen" @close="logOpen = false" />
    </main>
  </div>
</template>

<style scoped>
.remote { width: 100%; height: 100%; display: flex; flex-direction: column; min-height: 0; }
header { display: flex; align-items: center; gap: 14px; min-height: 64px; padding: 8px 24px; border-bottom: 1px solid var(--line); background: var(--panel); }
header img { object-fit: contain; object-position: left center; flex: none; }
header strong { font-weight: 600; }
.read-only { color: var(--text-dim); white-space: nowrap; }
.machine-state { padding: 6px 10px; margin-left: auto; border-radius: 6px; background: var(--go-soft); color: var(--go); font-weight: 600; }
.machine-state.stopped { background: var(--stop-soft); color: var(--stop); }
.machine-state.stale { background: var(--warn-soft); color: var(--warn); }
.machine-state.off { background: var(--bg-sunken); color: var(--text-dim); }
.job { display: flex; align-items: baseline; gap: 16px; margin: 10px 24px; }
h1 { margin: 0; font-size: 19px; overflow-wrap: anywhere; }
.notice { margin: 0; color: var(--text-dim); }
.notice.error { color: var(--stop); }
main { position: relative; flex: 1; min-height: 0; margin: 0 24px 12px; }
#dialogs, #workspace-dialogs { position: absolute; inset: 0; z-index: 20; pointer-events: none; }
@media (max-width: 920px) {
  header { flex-wrap: wrap; gap: 8px; padding-inline: 12px; }
  main, .job, .remote :deep(.dro) { margin-inline: 12px; }
  .remote :deep(.dro) { flex-wrap: wrap; }
  .remote :deep(.workspace-card) { grid-template-columns: minmax(0, 1fr); }
  .remote :deep(.inspector) { display: none; }
}
@media (max-width: 540px) {
  header img { width: 125px; }
  header strong { flex: 1; }
  .machine-state { margin-left: 0; }
  .job { display: block; }
  .remote :deep(.dro) { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 6px; padding: 6px; }
  .remote :deep(.basis) { grid-column: span 2; width: auto; padding: 0; }
  .remote :deep(.basis-toggle) { min-height: 44px; }
  .remote :deep(.display-units) { grid-area: 1 / 3; justify-content: flex-end; padding: 0; margin: 0; border: 0; }
  .remote :deep(.units button) { width: 36px; height: 44px; }
  .remote :deep(.axis-group) { border: 0; }
  .remote :deep(.remote-axis) { display: grid; width: 100%; height: auto; gap: 2px; padding: 2px 4px; }
  .remote :deep(.letter), .remote :deep(.number) { font-size: clamp(15px, 5vw, 20px); }
  .remote :deep(.reading) { width: auto; min-height: 35px; text-align: left; }
  .remote :deep(.marker) { position: static; }
  .remote :deep(.rotary) { grid-column: span 3; flex-wrap: wrap; padding-left: 0; }
  .remote :deep(.rotary-axis) { flex: 1 1 100px; grid-template-columns: auto auto; gap: 4px; }
  .remote :deep(.active-tool) { display: none; }
  .remote :deep(.status-row) { flex-wrap: wrap; }
}
</style>
