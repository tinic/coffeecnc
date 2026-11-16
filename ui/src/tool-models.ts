import { computed } from 'vue'
import { onResult, panel, send } from './machine'
import { createToolModelState } from './tool-model-state'

export const toolModelSettings = createToolModelState(panel, { send, onResult })
export const toolModels = toolModelSettings.state

export function savedProbe(number: number) {
  return panel.toolModels?.document?.assignments.find((entry) =>
    entry.tool.number === number && entry.family === 'touch-probe')
}

export const touchProbeLoaded = computed(() => {
  const number = panel.status?.tool.in_spindle ?? 0
  return savedProbe(number) !== undefined || toolModels.assignments.get(number)?.family === 'touch-probe'
})
