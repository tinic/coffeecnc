// The one panel store of the running page, connected to the executable or the simulator, plus
// the readings of its state that several components share.

import { computed, watch, type ComputedRef } from 'vue'

import { themeOverride, themePreference } from './appearance'
import { alternateTheme, decodeProgramLayout } from './operator-settings'
import { createOperatorSettingsState } from './operator-settings-state'
import { createTransport, isRemoteScreen } from './bridge'
import { connectionBlock, machineBlock } from './machine-block'
import { requiresHoming } from './machine-capabilities'
import type { AxisStatus, CommandBody, RotaryAxisStatus, Status, Tool } from './protocol'
import { createPanel, tabs, type PanelState, type ResultListener } from './store'

const instance = createPanel({ transport: createTransport() })

export const panel: PanelState = instance.state
export const operatorSettings = createOperatorSettingsState(panel, instance, undefined, {
  theme: themePreference, themeOverride,
  layoutOverride: decodeProgramLayout(new URLSearchParams(window.location.search).get('layout')),
})

export const chooseProgramLayout = operatorSettings.chooseProgramLayout

export function toggleTheme(): void {
  operatorSettings.chooseTheme(alternateTheme(themePreference.value))
}

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
  if (isRemoteScreen()) {
    panel.tab = 'program'
  } else if (tab !== undefined) {
    panel.tab = tab
  } else if (panel.simulated && parameters.get('scene') === 'probe') {
    const setup = parameters.get('setup')
    panel.tab = setup === 'setter' || setup === 'measure' ? 'tools' : 'probe'
  } else {
    // Choose once from the first controller snapshot; never move the operator's workspace
    // when later status updates arrive. An explicit URL workspace always wins.
    watch(() => panel.status, (status) => {
      if (status?.program.file) panel.tab = 'program'
    }, { once: true })
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

export const rotaryAxes: ComputedRef<RotaryAxisStatus[]> = computed(() => reported.value?.rotary_axes ?? [])
const configuredAxes = computed(() => [...axes.value, ...rotaryAxes.value])
export const allHomed: ComputedRef<boolean> = computed(
  () => configuredAxes.value.length > 0 && configuredAxes.value.every((axis) => axis.homed && !axis.homing),
)

export const homingActive: ComputedRef<boolean> = computed(() => configuredAxes.value.some((axis) => axis.homing))
export const homingRequired: ComputedRef<boolean> = computed(() => requiresHoming(panel.hello))

export function findTool(number: number): Tool | null {
  return panel.toolTable?.tools.find((tool) => tool.number === number) ?? null
}

/** Why nothing can be commanded at all, or null. */
export const linkBlock: ComputedRef<string | null> = computed(() => connectionBlock(panel.link))

/** Why the machine cannot move under manual control, or null. Mirrors what the machine rejects. */
export const manualBlock: ComputedRef<string | null> = computed(() => machineBlock(panel))

/** Why MDI, touch-off and coordinate system selection are unavailable, or null. */
export const mdiBlock: ComputedRef<string | null> = computed(() =>
  manualBlock.value ?? (!homingRequired.value || allHomed.value ? null : 'Axes are not homed'),
)
