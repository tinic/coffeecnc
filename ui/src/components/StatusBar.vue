<script setup lang="ts">
import { vTooltip } from '../tooltip'
import { vPopover } from '../popover'
import { computed, onBeforeUnmount, shallowRef, useTemplateRef, watch } from 'vue'
import { theme, toggleTheme } from '../appearance'
import { dismissNotice, live, onResult, panel, reported, send } from '../machine'
import type { CommandBody, Hello, Status } from '../protocol'
import { baseName, wcsName } from '../units'
import type { Tab } from '../store'
import AppIcon from './AppIcon.vue'

const emit = defineEmits<{ log: []; open: []; quit: []; settings: [] }>()
const status = reported
const menu = useTemplateRef<HTMLDetailsElement>('menu')
function openAppDialog(dialog: 'quit' | 'settings'): void {
  if (menu.value) {
    menu.value.open = false
    menu.value.querySelector('summary')?.focus({ preventScroll: true })
  }
  if (dialog === 'quit') emit('quit')
  else emit('settings')
}
type PowerCommand = Extract<CommandBody, { type: 'set_machine' }>
const pendingPower = shallowRef<{ command: PowerCommand; hello: Hello | null; before: Status | null; accepted: boolean } | null>(null)
const powerLabel = computed(() => status.value?.machine === 'estop' ? 'Reset E-stop' : status.value?.machine === 'on' ? 'Machine off' : 'Machine on')
const powerTarget = computed(() => status.value?.machine === 'off' ? 'on' : 'off')
const powerBlocked = computed(() => pendingPower.value !== null || !live.value || status.value === null)
function power(state: 'on' | 'off'): void {
  if (powerBlocked.value) return
  const command: PowerCommand = { type: 'set_machine', state }
  // Install before send: the browser simulator can answer synchronously.
  pendingPower.value = { command, hello: panel.hello, before: panel.status, accepted: false }
  send(command)
}
const removeResultListener = onResult((command, result) => {
  const pending = pendingPower.value
  if (pending?.command !== command) return
  pendingPower.value = result.outcome === 'accepted' ? { ...pending, accepted: true } : null
})
onBeforeUnmount(removeResultListener)
watch([pendingPower, (): Status | null => panel.status, (): Hello | null => panel.hello, live], () => {
  const pending = pendingPower.value
  if (pending === null) return
  if (!live.value || panel.hello !== pending.hello) { pendingPower.value = null; return }
  if (pending.accepted && panel.status !== pending.before && panel.status?.machine === pending.command.state) {
    pendingPower.value = null
    if (menu.value?.open) {
      menu.value.open = false
      menu.value.querySelector('summary')?.focus({ preventScroll: true })
    }
  }
}, { flush: 'sync' })
const tabs = computed(() => {
  const entries: [Tab, string][] = [['manual', 'Manual'], ['program', 'Machine'], ['tools', 'Tools']]
  if (panel.hello?.fixed_setter?.material_travel_mm && panel.hello.touch_probe_tool)
    entries.push(['probe', 'Touch off'])
  return entries
})
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
  if (panel.tab === 'probe') return 'Material touch-off'
  const file = status.value?.program.file
  if (!file) return 'No program loaded'
  const name = baseName(file).replace(/\.[^.]+$/, '').replace(/[-_]/g, ' ')
  return name.charAt(0).toUpperCase() + name.slice(1)
})
const subtitle = computed(() => {
  if (panel.tab === 'manual') return 'Jog · Home · Touch off · MDI'
  if (panel.tab === 'tools') return 'Tool table and work coordinate systems'
  if (panel.tab === 'probe') return 'Set the active work origin with the touch probe'
  const s = status.value
  return s?.program.file ? `${baseName(s.program.file)}   /   ${wcsName(s.offsets.wcs)}   /   Program in ${s.program_units}` : 'Open an NC program to inspect its toolpath'
})
</script>

<template>
  <header class="em-top">
    <div class="em-brand">
      <img v-show="theme === 'light'" class="brand-logo" src="/branding/coffeecnc-logo-light.svg" alt="CoffeeCNC" width="161" height="36" />
      <img v-show="theme === 'dark'" class="brand-logo" src="/branding/coffeecnc-logo-dark.svg" alt="CoffeeCNC" width="161" height="36" />
      <small>/ MILL</small>
    </div>
    <nav class="em-nav" aria-label="Workspace">
      <button v-for="[tab, label] in tabs" :key="tab" type="button" :aria-current="panel.tab === tab ? 'page' : undefined" @click="panel.tab = tab">{{ label }}</button>
    </nav>
    <div class="machine">
      <span v-tooltip="linkLabel" class="connection" :class="{ disconnected: !live }"><i class="dot"></i><span class="connection-label">{{ linkLabel }}</span></span>
      <button class="theme" type="button" :aria-label="`Switch to ${theme === 'light' ? 'dark' : 'light'} theme`" @click="toggleTheme">◐</button>
    </div>
  </header>
  <section class="jobbar">
    <div class="job">
      <div class="title-row"><h1>{{ title }}</h1><button v-if="panel.tab === 'program'" class="open" type="button" aria-label="Open program" :disabled="!live" @click="emit('open')"><AppIcon name="folder" /></button></div>
      <p class="subtitle">{{ subtitle }}</p>
    </div>
    <div class="machine-controls">
      <button class="estop" type="button" aria-label="Assert software E-stop" @click="send({ type: 'set_machine', state: 'estop' })"><AppIcon name="stop" />E-STOP</button>
      <button class="btn power-control" :class="{ go: status?.machine === 'off' }" type="button" :disabled="powerBlocked" @click="power(powerTarget)"><AppIcon name="power" />{{ powerLabel }}</button>
      <details ref="menu" v-popover class="state-menu">
        <summary v-tooltip="'App menu'" class="app-menu-toggle" aria-label="App menu"><AppIcon name="menu" /></summary>
        <div class="popover">
          <h3>Machine</h3>
          <div class="fact"><span>State</span><span class="state-pill" :class="state.tone"><i class="dot"></i>{{ state.label }}</span></div>
          <div class="fact"><span>Connection</span><span>{{ linkLabel }}</span></div>
          <div class="fact"><span>Task</span><span>{{ status?.mode ?? '—' }} · {{ status?.interp ?? '—' }}</span></div>
          <div class="fact"><span>Active codes</span><span class="modal-codes mono">{{ status?.active_codes.join(' ') ?? '—' }}</span></div>
          <div v-if="status && status.offsets.rotation_xy !== 0" class="fact"><span>XY rotation</span><span>{{ status.offsets.rotation_xy }}°</span></div>
          <p v-if="panel.notice" class="notice">{{ panel.notice.text }}</p>
          <div class="actions">
            <button class="btn menu-log" type="button" @click="emit('log')"><AppIcon name="list" />Message log · <span class="log-count mono">{{ panel.log.length }}</span></button>
          </div>
          <p v-if="pendingPower" class="notice" role="status">Waiting for machine {{ pendingPower.command.state }} confirmation…</p>
          <div class="application-actions">
            <button class="btn settings" type="button" @click="openAppDialog('settings')"><AppIcon name="settings" />Settings</button>
            <button class="btn quit" type="button" @click="openAppDialog('quit')"><AppIcon name="power" />Quit CoffeeCNC…</button>
          </div>
        </div>
      </details>
    </div>
  </section>
  <section class="message-area" aria-label="Machine messages">
    <div class="message-band" :class="panel.notice?.level ?? 'empty'">
      <span class="message-label">{{ panel.notice?.level === 'error' ? '!' : 'i' }}</span>
      <div class="message-copy">
        <p role="alert" aria-atomic="true"><template v-if="panel.notice?.level === 'error'"><strong v-if="panel.notice.context">{{ panel.notice.context }} · </strong>{{ panel.notice.text }}<span v-if="panel.notice.count > 1"> (×{{ panel.notice.count }})</span></template></p>
        <p role="status" aria-atomic="true"><template v-if="panel.notice?.level === 'info'">{{ panel.notice.text }}</template></p>
        <p v-if="!panel.notice" class="no-message">No new messages</p>
      </div>
      <button class="message-log" type="button" @click="emit('log')">Message log · <span class="log-count mono">{{ panel.log.length }}</span></button>
      <button v-if="panel.notice" v-tooltip="'Dismiss this message; keep it in the message log'" class="dismiss" type="button" @click="dismissNotice">Dismiss</button>
    </div>
  </section>
</template>

<style scoped>
.em-top { display: grid; grid-template-columns: 1fr auto 1fr; align-items: center; gap: 20px; padding: 10px 24px; border-bottom: 1px solid var(--line); background: var(--panel); }
.em-brand { display: flex; align-items: center; gap: 10px; white-space: nowrap; }
.brand-logo { display: block; flex: none; width: 161px; height: 36px; }
.em-brand small { font-size: 11px; font-weight: 400; letter-spacing: 1px; color: var(--text-dim); }
.em-nav { display: flex; padding: 4px; gap: 3px; border-radius: 10px; background: var(--bg); }
.em-nav button { min-width: 94px; min-height: 36px; padding: 8px 19px; border: 0; border-radius: 7px; background: none; font-weight: 500; color: var(--text-dim); cursor: pointer; }
.em-nav button[aria-current] { background: var(--panel); box-shadow: 0 1px 4px #00000010; color: var(--text); }
.machine { display: flex; align-items: center; justify-content: flex-end; gap: 10px; }
.connection { display: flex; align-items: center; gap: 8px; font-size: 11px; color: var(--text-dim); }
.dot { display: inline-block; width: 6px; height: 6px; border-radius: 50%; background: var(--go); flex: none; }
.disconnected { color: var(--stop); }.disconnected .dot { background: var(--stop); }
.theme { width: 32px; height: 36px; background: none; border: 0; color: var(--text-dim); font-size: 18px; cursor: pointer; }
.estop { display: flex; align-items: center; justify-content: center; gap: 9px; flex: none; min-width: 132px; height: 48px; padding: 0 15px; background: #b9202b; border: 2px solid #941822; border-radius: 9px; box-shadow: inset 0 -2px 0 #74131b; color: #fff; font-size: 17px; font-weight: 700; cursor: pointer; }
.estop:hover { background: #a51924; }.estop:active { box-shadow: none; transform: translateY(1px); }.estop:focus-visible { outline: 3px solid var(--accent); outline-offset: 3px; }
.estop :deep(svg) { width: 22px; height: 22px; }
.machine-controls { display: flex; align-items: center; gap: 12px; flex: none; }.power-control { min-width: 152px; min-height: 48px; font-size: 15px; }
.jobbar { display: flex; justify-content: space-between; align-items: center; padding: 12px 24px; gap: 20px; }
.job { min-width: 0; }.title-row { display: flex; gap: 12px; align-items: center; }
h1 { margin: 0; font-size: 23px; line-height: 1.2; letter-spacing: -.7px; font-weight: 550; }
.subtitle { margin: 5px 0 0; font-size: 12px; color: var(--text-dim); }
.open { display: flex; align-items: center; justify-content: center; width: 36px; height: 32px; border: 0; background: none; color: var(--text-dim); cursor: pointer; }
.open:disabled { opacity: .4; }
.state-menu { position: relative; flex: none; }
.app-menu-toggle { display: flex; align-items: center; justify-content: center; width: 48px; height: 48px; border: 1px solid var(--line); border-radius: 9px; background: var(--panel); color: var(--text); }
.app-menu-toggle :deep(svg) { width: 22px; height: 22px; }
.app-menu-toggle:hover, .state-menu[open] .app-menu-toggle { background: var(--bg); }
.state-pill { display: inline-flex; align-items: center; gap: 8px; }
.state-pill.warn .dot { background: var(--warn); }.state-pill.stop { color: var(--stop); }.state-pill.stop .dot { background: var(--stop); }
.popover { top: calc(100% + 8px); }.notice { font-size: 12px; color: var(--text-dim); }
.application-actions { display: grid; gap: 8px; margin-top: 12px; padding-top: 12px; border-top: 1px solid var(--line); }
.quit, .menu-log, .settings { width: 100%; justify-content: flex-start; min-height: 48px; }
.modal-codes { max-width: 180px; font-size: 10px; text-align: right; }
.message-area { flex: none; height: 58px; margin: 0 24px 12px; }
.message-band { display: flex; align-items: center; gap: 12px; height: 100%; padding: 5px 10px; border: 1px solid var(--line); border-radius: 9px; background: var(--panel); }
.message-band.error { color: var(--stop); background: var(--stop-soft); border-color: var(--stop); border-left-width: 5px; }
.message-band.empty { color: var(--text-dim); border-color: transparent; background: transparent; }
.message-label { display: grid; place-items: center; flex: none; width: 24px; height: 24px; border: 1px solid currentColor; border-radius: 50%; font-weight: 700; }
.message-copy { flex: 1; min-width: 0; }.message-copy p { margin: 0; font-size: 13px; line-height: 20px; overflow-wrap: anywhere; display: -webkit-box; -webkit-line-clamp: 2; -webkit-box-orient: vertical; overflow: hidden; }
.message-copy p:empty { height: 0; }.no-message { color: var(--text-dim); }
.message-log, .dismiss { flex: none; min-height: 44px; padding: 0 10px; border: 1px solid var(--line); border-radius: 7px; background: var(--panel); color: var(--text-dim); font-size: 12px; cursor: pointer; }
.log-count { display: inline-block; width: 3ch; min-width: 3ch; text-align: right; }
.message-log:hover, .dismiss:hover { color: var(--text); background: var(--bg); }
.message-log:focus-visible, .dismiss:focus-visible { outline: 2px solid var(--accent); outline-offset: 1px; }
@media (max-height: 820px) { .em-top { padding-block: 6px; }.jobbar { padding-block: 8px; }.message-area { height: 54px; margin-bottom: 8px; } }
@media (max-width: 1100px) { .connection-label { display: none; } }
@media (max-height: 760px) { .em-top { padding-block: 4px; }.jobbar { padding-block: 4px; }.message-area { height: 44px; }.message-copy p { line-height: 16px; } }
</style>
