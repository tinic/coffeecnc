import { onResult, panel, send } from './machine'
import { createToolModelState } from './tool-model-state'

export const toolModelSettings = createToolModelState(panel, { send, onResult })
export const toolModels = toolModelSettings.state
