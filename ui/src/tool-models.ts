import { computed } from 'vue'
import { t } from './i18n'
import { onResult, panel, send } from './machine'
import { createToolModelState } from './tool-model-state'
import type { SavedToolModelAssignment } from './tool-model-settings'

export const toolModelSettings = createToolModelState(panel, { send, onResult })
export const toolModels = toolModelSettings.state

export function savedProbe(number: number): SavedToolModelAssignment | undefined {
  return panel.toolModels?.document?.assignments.find((entry) =>
    entry.tool.number === number && entry.family === 'touch-probe')
}

export const touchProbeLoaded = computed(() => {
  const number = panel.status?.tool.in_spindle ?? 0
  if (number > 0 && number === panel.hello?.touch_probe_tool) return true
  return savedProbe(number) !== undefined || toolModels.assignments.get(number)?.family === 'touch-probe'
})

// The name shown for a tool without a description: the machine's special tools have one.
export function defaultToolName(number: number): string {
  if (number === panel.hello?.fixed_setter?.reference_tool) return t('tools.shared.setterReference')
  if (number === panel.hello?.touch_probe_tool || savedProbe(number) !== undefined) return t('tools.family.touchProbe')
  return ''
}
