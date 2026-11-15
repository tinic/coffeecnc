import { reactive, watch } from 'vue'
import type { CommandBody, Hello, Tool } from './protocol'
import type { Panel, PanelState } from './store'
import { decodeToolDefaults, factoryToolDefaults, toolIdentity,
  type ToolDefaults, type ToolFamily, type ToolModelAssignment, type ToolOverrides } from './tool-model'
import { decodeToolModelDocument, matchingToolModelAssignments, toolModelBinding, validMachineId } from './tool-model-settings'

const storageKey = 'coffeecnc.tool-model-defaults.v1'
export type ModelStorage = 'loading' | 'browser' | 'session_only' | 'available' | 'unavailable'
export interface ModelEditContext { hello: Hello | null; revision: number | null }
export interface ModelSaveResult { saved: boolean; reason: string }
type SaveCommand = Extract<CommandBody, { type: 'set_tool_model_defaults' | 'set_tool_model_assignment' }>
export interface ToolModelState {
  defaults: ToolDefaults
  assignments: Map<number, ToolModelAssignment>
  storage: ModelStorage
  storageReason: string
  storageError: string
  revision: number | null
  saving: boolean
  saved: string
  /** A timed-out write needs a fresh authoritative snapshot before another save. */
  uncertain: boolean
}
export interface ToolModelSettings {
  state: ToolModelState
  context: () => ModelEditContext
  block: (edit: ModelEditContext) => string | null
  saveDefaults: (value: ToolDefaults, edit: ModelEditContext) => Promise<ModelSaveResult>
  saveAssignment: (tool: Tool, family: ToolFamily | null, overrides: ToolOverrides, edit: ModelEditContext) => Promise<ModelSaveResult>
  dispose: () => void
}

/** Native preferences never consult browser storage, including native simulation sessions. */
export function createToolModelState(panel: PanelState, connection: Pick<Panel, 'send' | 'onResult'>,
  browserStorage: () => Pick<Storage, 'getItem' | 'setItem'> = () => localStorage): ToolModelSettings {
  let defaults = factoryToolDefaults()
  if (panel.simulated) {
    try { defaults = decodeToolDefaults(browserStorage().getItem(storageKey)) } catch { /* Session defaults. */ }
  }
  const state = reactive<ToolModelState>({ defaults, assignments: new Map(), storage: panel.simulated ? 'browser' : 'loading',
    storageReason: '', storageError: '', revision: null, saving: false, saved: '', uncertain: false })
  let pending: { command: SaveCommand; context: ModelEditContext; resolve: (result: ModelSaveResult) => void } | null = null
  const stops: (() => void)[] = []

  function fail(reason: string): ModelSaveResult {
    state.storageError = reason
    state.saved = ''
    return { saved: false, reason }
  }
  function finish(result: ModelSaveResult): void {
    const request = pending
    pending = null
    state.saving = false
    request?.resolve(result)
  }
  function restoreAssignments(): void {
    if (state.storage !== 'available') return
    const snapshot = panel.toolModels
    state.assignments = snapshot?.availability === 'available' && panel.link === 'live' && panel.toolTable !== null
      ? matchingToolModelAssignments(snapshot.document, panel.toolTable.tools) : new Map()
  }
  stops.push(watch(() => panel.hello, (hello) => {
    if (pending !== null) finish(fail('The machine connection changed. This draft was not confirmed saved.'))
    state.assignments.clear()
    state.storageError = ''; state.saved = ''; state.uncertain = false; state.revision = null
    if (panel.simulated) return
    state.defaults = factoryToolDefaults()
    state.storageReason = ''
    const capability: unknown = hello === null ? undefined : Reflect.get(hello, 'tool_model_storage')
    if (hello === null) state.storage = 'loading'
    else if (capability === undefined) {
      state.storage = 'session_only'
      state.storageReason = 'This backend does not support saved tool models. Changes last until this page closes.'
    } else if (capability !== 1 || !(hello.machine_id === null || validMachineId(hello.machine_id))) {
      state.storage = 'unavailable'
      state.storageReason = 'The machine did not provide a valid tool model storage identity.'
    } else state.storage = 'loading'
  }, { immediate: true, flush: 'sync' }))
  stops.push(watch(() => panel.toolModels, (snapshot) => {
    if (panel.simulated || panel.hello?.tool_model_storage !== 1) return
    if (snapshot === null) {
      state.storage = 'loading'; state.revision = null; state.assignments.clear()
      return
    }
    state.storage = snapshot.availability
    state.storageReason = snapshot.reason
    state.revision = snapshot.availability === 'available' ? snapshot.revision : null
    state.uncertain = false
    if (snapshot.availability === 'available') state.defaults = snapshot.document.defaults
    else state.assignments.clear()
    restoreAssignments()
  }, { immediate: true, flush: 'sync' }))
  stops.push(watch(() => panel.toolTable, (table) => {
    if (state.storage === 'available') { restoreAssignments(); return }
    for (const [number, assignment] of state.assignments) {
      const tool = table?.tools.find((candidate) => candidate.number === number)
      if (tool === undefined || assignment.identity !== toolIdentity(tool)) state.assignments.delete(number)
    }
  }, { flush: 'sync' }))
  stops.push(watch(() => panel.link, restoreAssignments, { flush: 'sync' }))
  stops.push(connection.onResult((command, result) => {
    if (command !== pending?.command) return
    if (result.outcome !== 'accepted') {
      state.uncertain = result.outcome === 'timed_out'
      const reason = result.outcome === 'timed_out'
        ? 'No save confirmation. The write may still finish; wait for refreshed settings or reload CoffeeCNC before retrying.'
        : result.reason || 'Tool model settings could not be saved.'
      finish(fail(reason))
    } else if (panel.hello !== pending.context.hello || state.storage !== 'available' ||
      state.revision === null || state.revision <= pending.command.base_revision) {
      state.uncertain = true
      finish(fail('The save result arrived without updated settings. Reload CoffeeCNC to verify the saved values.'))
    } else {
      state.storageError = ''
      state.saved = `Tool models saved for ${pending.command.machine_id}.`
      finish({ saved: true, reason: '' })
    }
  }))

  function context(): ModelEditContext { return { hello: panel.hello, revision: state.revision } }
  function block(edit: ModelEditContext): string | null {
    if (panel.hello !== edit.hello) return 'The machine connection changed. Close this editor and reopen it.'
    if (state.saving) return 'A tool model save is already in progress.'
    if (state.uncertain) return state.storageError
    if (state.storage === 'loading') return 'Waiting for tool model settings.'
    if (state.storage === 'unavailable') return state.storageReason || 'Tool model storage is unavailable.'
    if (state.storage === 'available' && state.revision !== edit.revision) {
      return 'Saved settings changed while this editor was open. Close and reopen it to review the current values.'
    }
    return null
  }
  function write(command: SaveCommand, edit: ModelEditContext): Promise<ModelSaveResult> {
    state.saving = true; state.storageError = ''; state.saved = ''
    return new Promise((resolve) => {
      // Install before send: a local bridge refusal may deliver its result synchronously.
      pending = { command, context: edit, resolve }
      try { connection.send(command) }
      catch { finish(fail('The tool model save could not be sent.')) }
    })
  }
  function saveDefaults(value: ToolDefaults, edit: ModelEditContext): Promise<ModelSaveResult> {
    const reason = block(edit)
    if (reason !== null) return Promise.resolve(fail(reason))
    const document = decodeToolModelDocument({ version: 1, defaults: value, assignments: [] })
    if (document === null) return Promise.resolve(fail('Tool model defaults contain invalid dimensions.'))
    if (state.storage === 'available' && panel.toolModels?.availability === 'available') {
      return write({ type: 'set_tool_model_defaults', machine_id: panel.toolModels.machine_id,
        base_revision: panel.toolModels.revision, defaults: document.defaults }, edit)
    }
    state.defaults = document.defaults
    state.storageError = ''
    if (state.storage === 'browser') {
      try { browserStorage().setItem(storageKey, JSON.stringify({ version: 1, defaults: document.defaults })) }
      catch { return Promise.resolve(fail('Browser storage is unavailable. These defaults last only until this page closes.')) }
    }
    state.saved = state.storage === 'browser' ? 'Tool defaults saved in this browser.' : 'Tool defaults applied for this page session.'
    return Promise.resolve({ saved: true, reason: '' })
  }
  function saveAssignment(tool: Tool, family: ToolFamily | null, overrides: ToolOverrides,
    edit: ModelEditContext): Promise<ModelSaveResult> {
    const reason = block(edit)
    if (reason !== null) return Promise.resolve(fail(reason))
    const binding = toolModelBinding(tool)
    if (binding === null) return Promise.resolve(fail('The current tool row cannot be saved as a model binding.'))
    if (!panel.toolTable?.tools.some((row) => toolIdentity(row) === toolIdentity(tool)) ||
      !panel.simulated && panel.link !== 'live') {
      return Promise.resolve(fail('A current matching tool table is required. Reopen this editor when the machine connection is live.'))
    }
    const document = decodeToolModelDocument({ version: 1, defaults: state.defaults,
      assignments: family === null ? [] : [{ tool: binding, family, overrides }] })
    if (document === null || family === null && Object.keys(overrides).length !== 0) {
      return Promise.resolve(fail('The tool model assignment contains invalid dimensions.'))
    }
    if (state.storage === 'available' && panel.toolModels?.availability === 'available') {
      return write({ type: 'set_tool_model_assignment', machine_id: panel.toolModels.machine_id,
        base_revision: panel.toolModels.revision, tool: binding, family, overrides: { ...overrides } }, edit)
    }
    if (family === null) state.assignments.delete(tool.number)
    else state.assignments.set(tool.number, { identity: toolIdentity(tool), family, overrides: { ...overrides } })
    state.storageError = ''
    state.saved = 'Tool model applied for this page session.'
    return Promise.resolve({ saved: true, reason: '' })
  }
  return { state, context, block, saveDefaults, saveAssignment,
    dispose: (): void => {
      for (const stop of stops) stop()
      if (pending !== null) finish(fail('This settings view closed before save confirmation.'))
    } }
}
