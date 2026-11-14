// The one panel store of the running page, connected to the executable or the simulator, plus
// the readings of its state that several components share.

import { computed, type ComputedRef } from 'vue'

import { createTransport } from './bridge'
import type { AxisStatus, CommandBody, Tool } from './protocol'
import { createPanel, tabs, type PanelState, type ResultListener } from './store'

const instance = createPanel({ transport: createTransport() })

export const panel: PanelState = instance.state

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

export const axes: ComputedRef<AxisStatus[]> = computed(() => panel.status?.axes ?? [])

export const allHomed: ComputedRef<boolean> = computed(
  () => axes.value.length > 0 && axes.value.every((axis) => axis.homed),
)

export const homingActive: ComputedRef<boolean> = computed(() => axes.value.some((axis) => axis.homing))

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
  return allHomed.value ? null : 'Axes are not homed'
})
