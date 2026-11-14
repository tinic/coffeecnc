import { reactive, watch } from 'vue'
import { panel } from './machine'
import { decodeToolDefaults, toolIdentity, type ToolDefaults, type ToolModelAssignment } from './tool-model'

const storageKey = 'coffeecnc.tool-model-defaults.v1'
function readDefaults(): ToolDefaults {
  try { return decodeToolDefaults(localStorage.getItem(storageKey)) }
  catch { return decodeToolDefaults(null) }
}

export const toolModels = reactive({ defaults: readDefaults(), assignments: new Map<number, ToolModelAssignment>(), storageError: '' })

export function persistToolDefaults(defaults: ToolDefaults): void {
  toolModels.defaults = defaults
  try {
    localStorage.setItem(storageKey, JSON.stringify({ version: 1, defaults }))
    toolModels.storageError = ''
  } catch { toolModels.storageError = 'Browser storage is unavailable. Defaults will last only until this page closes.' }
}

// Per-tool assignments stay page-local until machine-scoped native configuration is available.
// Prune changed/deleted tools so a reused T number cannot revive an earlier assignment.
watch(() => panel.toolTable, (table) => {
  for (const [number, assignment] of toolModels.assignments) {
    const tool = table?.tools.find((candidate) => candidate.number === number)
    if (tool === undefined || assignment.identity !== toolIdentity(tool)) toolModels.assignments.delete(number)
  }
})
watch(() => panel.hello, () => { toolModels.assignments.clear() })
