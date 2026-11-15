<script setup lang="ts">
import { computed } from 'vue'
import { theme, toggleTheme } from '../appearance'
import { dismissNotice, live, panel, reported, send } from '../machine'
import { baseName, wcsName } from '../units'
import type { Tab } from '../store'
import AppIcon from './AppIcon.vue'

const emit = defineEmits<{ log: []; open: [] }>()
const status = reported
const tabs: readonly [Tab, string][] = [['manual', 'Manual'], ['program', 'Machine'], ['tools', 'Tools']]
const linkLabel = computed(() => {
  switch (panel.link) {
    case 'live': return panel.simulated ? '3-axis mill · Simulator' : panel.hello?.machine_name ?? 'Connected'
    case 'connecting': return 'Waiting for connection'
    case 'silent': return 'No status updates'
    case 'stale': return 'Status is stale'
    case 'down': return 'Disconnected'
    case 'incompatible': return 'Protocol mismatch'
  }
})
const state = computed(() => {
  const s = status.value
  if (!live.value) return { label: linkLabel.value, tone: 'stop' }
  if (s?.machine === 'estop') return { label: 'E-stop active', tone: 'stop' }
  if (s?.machine === 'off') return { label: 'Machine off', tone: 'warn' }
  if (s?.wait.kind === 'tool_change') return { label: 'Tool change required', tone: 'warn' }
  if (s?.interp === 'paused') return { label: 'Paused', tone: 'warn' }
  if (s?.interp !== 'idle') return { label: s?.mode === 'mdi' ? 'Executing MDI' : 'Program running', tone: 'go' }
  return { label: 'Machine idle', tone: 'go' }
})
const title = computed(() => {
  if (panel.tab === 'manual') return 'Manual setup'
  if (panel.tab === 'tools') return 'Tools & offsets'
  const file = status.value?.program.file
  if (!file) return 'No program loaded'
  const name = baseName(file).replace(/\.[^.]+$/, '').replace(/[-_]/g, ' ')
  return name.charAt(0).toUpperCase() + name.slice(1)
})
const subtitle = computed(() => {
  if (panel.tab === 'manual') return 'Jog · Home · Touch off · MDI'
  if (panel.tab === 'tools') return 'Tool table and work coordinate systems'
  const s = status.value
  return s?.program.file ? `${baseName(s.program.file)}   /   ${wcsName(s.offsets.wcs)}   /   Program in ${s.program_units}` : 'Open an NC program to inspect its toolpath'
})
</script>

<template>
  <header class="em-top">
    <div class="em-brand"><span class="brand-mark" aria-hidden="true"><i></i><i></i><i></i></span>CoffeeCNC <small>/ MILL</small></div>
    <nav class="em-nav" aria-label="Workspace">
      <button v-for="[tab, label] in tabs" :key="tab" type="button" :aria-current="panel.tab === tab ? 'page' : undefined" @click="panel.tab = tab">{{ label }}</button>
    </nav>
    <div class="machine">
      <span class="connection" :class="{ disconnected: !live }" :title="linkLabel"><i class="dot"></i><span class="connection-label">{{ linkLabel }}</span></span>
      <button class="estop" type="button" title="Assert LinuxCNC software E-stop" @click="send({ type: 'set_machine', state: 'estop' })">E-stop</button>
      <button class="theme" type="button" :aria-label="`Switch to ${theme === 'light' ? 'dark' : 'light'} theme`" @click="toggleTheme">◐</button>
    </div>
  </header>
  <section class="jobbar">
    <div class="job">
      <div class="title-row"><h1>{{ title }}</h1><button v-if="panel.tab === 'program'" class="open" type="button" aria-label="Open program" :disabled="!live" @click="emit('open')"><AppIcon name="folder" /></button></div>
      <p class="subtitle">{{ subtitle }}</p>
      <div v-if="panel.notice?.level === 'error'" class="error-row">
        <p class="error" role="alert">{{ panel.notice.text }}</p>
        <button class="dismiss" type="button" title="Dismiss this message; keep it in the message log" @click="dismissNotice">Dismiss</button>
      </div>
    </div>
    <details class="state-menu">
      <summary class="state-pill" :class="state.tone"><i class="dot"></i>{{ state.label }}<AppIcon name="down" /></summary>
      <div class="popover">
        <h3>Machine</h3>
        <div class="fact"><span>Connection</span><span>{{ linkLabel }}</span></div>
        <div class="fact"><span>Task</span><span>{{ status?.mode ?? '—' }} · {{ status?.interp ?? '—' }}</span></div>
        <div class="fact"><span>Active codes</span><span class="modal-codes mono">{{ status?.active_codes.join(' ') ?? '—' }}</span></div>
        <div v-if="status && status.offsets.rotation_xy !== 0" class="fact"><span>XY rotation</span><span>{{ status.offsets.rotation_xy }}°</span></div>
        <p v-if="panel.notice" class="notice">{{ panel.notice.text }}</p>
        <div class="actions">
          <button v-if="status?.machine === 'estop' || status?.machine === 'off'" class="btn" type="button" :disabled="!live" @click="send({ type: 'set_machine', state: 'off' })">Reset E-stop</button>
          <button v-if="status?.machine === 'off'" class="btn" type="button" :disabled="!live" @click="send({ type: 'set_machine', state: 'on' })">Machine on</button>
          <button v-else-if="status?.machine === 'on'" class="btn" type="button" :disabled="panel.link === 'incompatible'" @click="send({ type: 'set_machine', state: 'off' })">Machine off</button>
          <button class="btn quiet" type="button" @click="emit('log')">Message log · {{ panel.log.length }}</button>
        </div>
      </div>
    </details>
  </section>
</template>

<style scoped>
.em-top { display: grid; grid-template-columns: 1fr auto 1fr; align-items: center; gap: 20px; padding: 18px 24px; border-bottom: 1px solid var(--line); background: var(--panel); }
.em-brand { display: flex; align-items: center; gap: 10px; font-size: 17px; letter-spacing: 3px; font-weight: 700; white-space: nowrap; }
.em-brand small { font-size: 11px; font-weight: 400; letter-spacing: 1px; color: var(--text-dim); margin-left: 4px; }
.brand-mark { display: flex; align-items: end; gap: 3px; height: 21px; }
.brand-mark i { width: 3px; height: 21px; border-radius: 2px; background: var(--accent); }
.brand-mark i:nth-child(2) { height: 14px; }.brand-mark i:nth-child(3) { height: 7px; }
.em-nav { display: flex; padding: 4px; gap: 3px; border-radius: 10px; background: var(--bg); }
.em-nav button { min-width: 94px; min-height: 36px; padding: 8px 19px; border: 0; border-radius: 7px; background: none; font-weight: 500; color: var(--text-dim); cursor: pointer; }
.em-nav button[aria-current] { background: var(--panel); box-shadow: 0 1px 4px #00000010; color: var(--text); }
.machine { display: flex; align-items: center; justify-content: flex-end; gap: 10px; }
.connection { display: flex; align-items: center; gap: 8px; font-size: 11px; color: var(--text-dim); }
.dot { display: inline-block; width: 6px; height: 6px; border-radius: 50%; background: var(--go); flex: none; }
.disconnected { color: var(--stop); }.disconnected .dot { background: var(--stop); }
.theme { width: 32px; height: 36px; background: none; border: 0; color: var(--text-dim); font-size: 18px; cursor: pointer; }
.estop { min-height: 44px; padding: 0 10px; background: none; border: 1px solid transparent; border-radius: 7px; color: var(--stop); font-size: 11px; cursor: pointer; }.estop:hover { border-color: var(--stop); background: var(--stop-soft); }
.jobbar { display: flex; justify-content: space-between; align-items: center; padding: 19px 24px 16px; gap: 20px; }
.job { min-width: 0; }.title-row { display: flex; gap: 12px; align-items: center; }
h1 { margin: 0; font-size: 23px; line-height: 1.2; letter-spacing: -.7px; font-weight: 550; }
.subtitle { margin: 5px 0 0; font-size: 12px; color: var(--text-dim); }
.open { display: flex; align-items: center; justify-content: center; width: 36px; height: 32px; border: 0; background: none; color: var(--text-dim); cursor: pointer; }
.open:disabled { opacity: .4; }
.state-menu { position: relative; flex: none; }
.state-pill { display: flex; align-items: center; gap: 8px; padding: 9px 12px; border: 1px solid var(--line); border-radius: 30px; background: var(--panel); font-size: 12px; }
.state-pill :deep(svg) { width: 12px; height: 12px; color: var(--text-dim); }
.state-pill.warn .dot { background: var(--warn); }.state-pill.stop { color: var(--stop); }.state-pill.stop .dot { background: var(--stop); }
.popover { top: calc(100% + 8px); }.notice { font-size: 12px; color: var(--text-dim); }
.modal-codes { max-width: 180px; font-size: 10px; text-align: right; }
.error-row { display: flex; align-items: center; gap: 12px; margin-top: 6px; max-width: 70vw; }
.error { color: var(--stop); font-size: 12px; margin: 0; overflow-wrap: anywhere; }
.dismiss { flex: none; min-height: 36px; padding: 0 10px; border: 1px solid var(--line); border-radius: 7px; background: var(--panel); color: var(--text-dim); font-size: 12px; cursor: pointer; }
.dismiss:hover { color: var(--text); background: var(--bg); }
@media (max-height: 760px) { .em-top { padding-block: 12px; }.jobbar { padding-block: 12px; } }
@media (max-width: 1100px) { .connection-label { display: none; } }
</style>
