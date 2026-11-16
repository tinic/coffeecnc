// The one panel store of the running page, connected to the executable or the simulator, plus
// the readings of its state that several components share.

import { computed, watch, type ComputedRef } from 'vue'

import { createOperatorSettingsState } from './operator-settings-state'
import { createTransport } from './bridge'
import { requiresHoming } from './machine-capabilities'
import type { AxisStatus, CommandBody, Status, Tool } from './protocol'
import { createPanel, tabs, type PanelState, type ResultListener } from './store'

const instance = createPanel({ transport: createTransport() })

export const panel: PanelState = instance.state
export const operatorSettings = createOperatorSettingsState(panel, instance)

export function send(command: CommandBody): number {
  return instance.send(command)
}

export function onResult(listener: ResultListener): () => void {
  return instance.onResult(listener)
}

export function dismissNotice(): void {
  instance.dismissNotice()
}

export const setSimulatorFault = instance.setSimulatorFault

export function start(): void {
  const parameters = new URLSearchParams(window.location.search)
  const tab = tabs.find((candidate) => candidate === parameters.get('tab'))
  if (tab !== undefined) {
    panel.tab = tab
  } else {
    // Choose once from the first controller snapshot; never move the operator's workspace
    // when later status updates arrive. An explicit URL workspace always wins.
    watch(() => panel.status, (status) => {
      if (status?.program.file) panel.tab = 'program'
    }, { once: true })
  }
  const layout = (['split', 'text', 'path'] as const).find((candidate) => candidate === parameters.get('layout'))
  if (layout !== undefined) {
    panel.programLayout = layout
  }
  instance.start()
  send({ type: 'ready' })
}

/** The status is from a running LinuxCNC and fresh. Nothing else may be shown as live. */
export const live: ComputedRef<boolean> = computed(() => panel.link === 'live')

/**
 * The status, when it says anything about the machine: with the link down every field is a
 * placeholder, and placeholders are not shown as values.
 */
export const reported: ComputedRef<Status | null> = computed(() => (panel.link === 'down' ? null : panel.status))

export const axes: ComputedRef<AxisStatus[]> = computed(() => reported.value?.axes ?? [])

export const allHomed: ComputedRef<boolean> = computed(
  () => axes.value.length > 0 && axes.value.every((axis) => axis.homed),
)

export const homingActive: ComputedRef<boolean> = computed(() => axes.value.some((axis) => axis.homing))
export const homingRequired: ComputedRef<boolean> = computed(() => requiresHoming(panel.hello))

export function findTool(number: number): Tool | null {
  return panel.toolTable?.tools.find((tool) => tool.number === number) ?? null
}

/** Why nothing can be commanded at all, or null. */
export const linkBlock: ComputedRef<string | null> = computed(() => {
  switch (panel.link) {
    case 'live':
      return null
    case 'connecting':
      return 'No status from the machine yet'
    case 'silent':
      return 'No updates from the machine'
    case 'stale':
      return 'LinuxCNC has stopped updating its status'
    case 'down':
      return 'No connection to LinuxCNC'
    case 'incompatible':
      return 'Panel and backend protocol versions do not match'
  }
})

/** Why the machine cannot move under manual control, or null. Mirrors what the machine rejects. */
export const manualBlock: ComputedRef<string | null> = computed(() => {
  const status = panel.status
  if (linkBlock.value !== null || status === null) {
    return linkBlock.value ?? 'No status from the machine yet'
  }
  if (status.machine === 'estop') {
    return 'Machine is in E-stop'
  }
  if (status.machine === 'off') {
    return 'Machine is off'
  }
  if (status.interp !== 'idle') {
    return status.mode === 'mdi' ? 'An MDI command is running' : 'A program is running'
  }
  return null
})

/** Why MDI, touch-off and coordinate system selection are unavailable, or null. */
export const mdiBlock: ComputedRef<string | null> = computed(() => {
  if (manualBlock.value !== null) {
    return manualBlock.value
  }
  if (homingActive.value) {
    return 'Homing is in progress'
  }
  return !homingRequired.value || allHomed.value ? null : 'Axes are not homed'
})
