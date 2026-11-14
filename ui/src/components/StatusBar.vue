<script setup lang="ts">
// Always visible: which machine, how fresh its status is, what state it is in, which work system
// and tool are active, and the last thing the operator was told.
import { computed } from 'vue'

import { dismissNotice, findTool, live, panel, reported, send } from '../machine'
import { formatClock, wcsName } from '../units'
import AppIcon from './AppIcon.vue'

const emit = defineEmits<{ log: [] }>()

const status = reported

const link = computed((): { text: string; tone: 'go' | 'warn' | 'stop'; title: string } => {
  switch (panel.link) {
    case 'connecting':
      return { text: 'Waiting for status', tone: 'warn', title: 'No status has arrived from the machine yet.' }
    case 'live':
      return { text: 'Live', tone: 'go', title: 'LinuxCNC is updating its status.' }
    case 'silent':
      return {
        text: `No updates for ${String(Math.floor(panel.silentFor / 1000))} s`,
        tone: 'stop',
        title: 'No status has arrived for over a second. What is shown is the last status received, not the present state.',
      }
    case 'stale':
      return {
        text: 'Status stale',
        tone: 'stop',
        title: 'LinuxCNC has stopped updating its status. What is shown is the last status it wrote.',
      }
    case 'down':
      return { text: 'No connection', tone: 'stop', title: 'There is no connection to LinuxCNC.' }
  }
})

const machineLabel = computed(() => {
  switch (status.value?.machine) {
    case 'estop':
      return 'E-stop'
    case 'off':
      return 'Off'
    case 'on':
      return 'On'
    case undefined:
      return '—'
  }
})

const tool = computed(() => {
  const number = status.value?.tool.in_spindle ?? 0
  if (status.value === null) {
    return { name: '—', detail: '' }
  }
  if (number === 0) {
    return { name: 'No tool', detail: '' }
  }
  const row = findTool(number)
  return { name: `T${String(number)}`, detail: row === null ? 'not in tool table' : row.comment }
})

const prepared = computed(() => status.value?.tool.prepared ?? null)

const notice = computed(() => panel.notice)
const errorCount = computed(() => panel.log.filter((entry) => entry.level === 'error').length)
</script>

<template>
  <header class="statusbar">
    <div class="cell brand">
      <span class="name">{{ panel.hello?.machine_name ?? 'CoffeeCNC' }}</span>
      <span class="sub">
        <span v-if="panel.simulated" class="chip warn" title="No LinuxCNC is behind this session.">Simulator</span>
        <span class="chip" :class="link.tone" :title="link.title"><span class="dot"></span>{{ link.text }}</span>
      </span>
    </div>

    <div class="cell machine" :class="{ distrust: !live }">
      <span class="label">Machine</span>
      <span class="value state" :class="status?.machine">{{ machineLabel }}</span>
    </div>
    <div class="controls">
      <button
        v-if="status?.machine === 'estop'"
        class="btn"
        type="button"
        title="Clears LinuxCNC's software E-stop. The machine stays off."
        @click="send({ type: 'set_machine', state: 'off' })"
      >
        Reset E-stop
      </button>
      <button
        v-else-if="status?.machine === 'off'"
        class="btn go"
        type="button"
        @click="send({ type: 'set_machine', state: 'on' })"
      >
        <AppIcon name="power" />Machine on
      </button>
      <button
        v-else
        class="btn"
        type="button"
        :disabled="status === null"
        title="Switches the machine off. Aborts any motion."
        @click="send({ type: 'set_machine', state: 'off' })"
      >
        <AppIcon name="power" />Machine off
      </button>
      <button
        class="btn danger"
        type="button"
        :disabled="status?.machine === 'estop'"
        title="LinuxCNC's software E-stop. It is not the machine's E-stop circuit."
        @click="send({ type: 'set_machine', state: 'estop' })"
      >
        E-stop
      </button>
    </div>

    <div class="cell" :class="{ distrust: !live }">
      <span class="label" title="LinuxCNC task mode and interpreter state, as reported">Task</span>
      <span class="value">{{ status?.mode ?? '—' }} · {{ status?.interp ?? '—' }}</span>
    </div>
    <div class="cell" :class="{ distrust: !live }">
      <span class="label" title="Active work coordinate system">WCS</span>
      <span class="value mono">{{ status ? wcsName(status.offsets.wcs) : '—' }}</span>
    </div>
    <div class="cell toolcell" :class="{ distrust: !live }">
      <span class="label">Tool in spindle<template v-if="prepared !== null"> · T{{ prepared }} prepared</template></span>
      <span class="value">
        <span class="mono">{{ tool.name }}</span>
        <span v-if="tool.detail" class="detail">{{ tool.detail }}</span>
      </span>
    </div>

    <div class="message" :class="notice?.level">
      <template v-if="notice">
        <AppIcon :name="notice.level === 'error' ? 'warning' : 'info'" />
        <span class="text" :title="notice.text">
          <span class="when mono">{{ formatClock(notice.time) }}</span>
          {{ notice.text }}<template v-if="notice.count > 1"> (×{{ notice.count }})</template>
        </span>
        <button class="btn quiet square" type="button" title="Dismiss. The message stays in the log." @click="dismissNotice">
          <AppIcon name="close" />
        </button>
      </template>
      <span v-else class="none">No message</span>
      <button class="btn" type="button" title="Open the message log" @click="emit('log')">
        <AppIcon name="list" />Log<template v-if="errorCount > 0"> · {{ errorCount }}</template>
      </button>
    </div>
  </header>
</template>

<style scoped>
.statusbar {
  display: flex;
  align-items: stretch;
  gap: 12px;
  height: 60px;
  padding: 8px 12px;
  border-bottom: 1px solid var(--line);
  background: var(--panel);
}

.cell {
  display: flex;
  flex-direction: column;
  justify-content: center;
  gap: 3px;
  min-width: 0;
  white-space: nowrap;
}

.cell.distrust .value {
  color: var(--text-faint);
}

.brand {
  flex: none;
}

.name {
  font-weight: 600;
}

.sub {
  display: flex;
  gap: 6px;
}

.dot {
  width: 7px;
  height: 7px;
  border-radius: 50%;
  background: currentColor;
}

.value {
  display: flex;
  align-items: baseline;
  gap: 6px;
  font-size: 16px;
  font-weight: 600;
}

.state.estop {
  color: var(--stop);
}

.state.off {
  color: var(--warn);
}

.state.on {
  color: var(--go);
}

.machine {
  width: 62px;
}

.controls {
  display: flex;
  align-items: center;
  gap: 6px;
}

.toolcell {
  flex: 0 1 190px;
}

.detail {
  overflow: hidden;
  font-size: 13px;
  font-weight: 400;
  color: var(--text-dim);
  text-overflow: ellipsis;
}

.message {
  display: flex;
  flex: 1 1 260px;
  align-items: center;
  justify-content: flex-end;
  gap: 8px;
  min-width: 0;
  padding-left: 10px;
  border-left: 1px solid var(--line);
}

.message.error {
  color: var(--stop);
}

.message .text {
  display: -webkit-box;
  flex: 1;
  overflow: hidden;
  font-size: 13px;
  line-height: 1.3;
  color: var(--text);
  -webkit-box-orient: vertical;
  -webkit-line-clamp: 2;
  line-clamp: 2;
}

.message.error .text {
  font-weight: 600;
}

.message .btn {
  color: var(--text);
}

.when {
  margin-right: 4px;
  font-size: 12px;
  font-weight: 400;
  color: var(--text-dim);
}

.none {
  flex: 1;
  color: var(--text-faint);
}
</style>
