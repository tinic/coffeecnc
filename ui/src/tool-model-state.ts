import { reactive, watch } from 'vue'
import { t } from './i18n'
import { translateMessage } from './i18n/messages'
import type { CommandBody, Hello, Tool } from './protocol'
import type { Panel, PanelState } from './store'
import type { ToolHolder } from './tool-holder'
import { decodeToolDefaults, factoryToolDefaults, toolIdentity,
  type ToolDefaults, type ToolFamily, type ToolModelAssignment, type ToolOverrides } from './tool-model'
import { decodeToolModelDocument, matchingToolModelAssignments, toolModelBinding, validMachineId } from './tool-model-settings'
import { demoToolAssignment, hasDemoProbeFixture } from './sim/probe-fixture'

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
  saveAssignment: (tool: Tool, family: ToolFamily | null, overrides: ToolOverrides, edit: ModelEditContext, holder?: ToolHolder | null) => Promise<ModelSaveResult>
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
  const fixtureModels = new Set<number>()
  let waitingForFixtureTools = false

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
    if (pending !== null) finish(fail(t('tools.modelState.connectionChangedDraft')))
    state.assignments.clear()
    fixtureModels.clear()
    waitingForFixtureTools = hasDemoProbeFixture(panel.simulated, hello)
    state.storageError = ''; state.saved = ''; state.uncertain = false; state.revision = null
    if (panel.simulated) return
    state.defaults = factoryToolDefaults()
    state.storageReason = ''
    const capability: unknown = hello === null ? undefined : Reflect.get(hello, 'tool_model_storage')
    if (hello === null) state.storage = 'loading'
    else if (capability === undefined) {
      state.storage = 'session_only'
      state.storageReason = t('tools.modelState.backendNoStorage')
    } else if (capability !== 1 || !(hello.machine_id === null || validMachineId(hello.machine_id))) {
      state.storage = 'unavailable'
      state.storageReason = t('tools.modelState.invalidIdentity')
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
      if (tool === undefined || assignment.identity !== toolIdentity(tool)) {
        const updated = tool !== undefined && fixtureModels.has(number) ? demoToolAssignment(tool) : undefined
        if (updated) state.assignments.set(number, updated)
        else {
          state.assignments.delete(number)
          fixtureModels.delete(number)
        }
      }
    }
    if (waitingForFixtureTools && table !== null) {
      waitingForFixtureTools = false
      for (const tool of table.tools) {
        const model = demoToolAssignment(tool)
        if (model) {
          fixtureModels.add(tool.number)
          state.assignments.set(tool.number, model)
        }
      }
    }
  }, { flush: 'sync' }))
  stops.push(watch(() => panel.link, restoreAssignments, { flush: 'sync' }))
  stops.push(connection.onResult((command, result) => {
    if (command !== pending?.command) return
    if (result.outcome !== 'accepted') {
      state.uncertain = result.outcome === 'timed_out'
      const reason = result.outcome === 'timed_out'
        ? t('tools.modelState.noConfirmation')
        : translateMessage(result.reason, result.refusal) || t('tools.modelState.couldNotSave')
      finish(fail(reason))
    } else if (panel.hello !== pending.context.hello || state.storage !== 'available' ||
      state.revision === null || state.revision <= pending.command.base_revision) {
      state.uncertain = true
      finish(fail(t('tools.modelState.noUpdatedSettings')))
    } else {
      state.storageError = ''
      state.saved = t('tools.modelState.savedFor', { machine: pending.command.machine_id })
      finish({ saved: true, reason: '' })
    }
  }))

  function context(): ModelEditContext { return { hello: panel.hello, revision: state.revision } }
  function block(edit: ModelEditContext): string | null {
    if (panel.hello !== edit.hello) return t('tools.modelState.connectionChanged')
    if (state.saving) return t('tools.modelState.inProgress')
    if (state.uncertain) return translateMessage(state.storageError)
    if (state.storage === 'loading') return t('tools.modelState.waiting')
    if (state.storage === 'unavailable') return translateMessage(state.storageReason) || t('tools.modelState.unavailable')
    if (state.storage === 'available' && state.revision !== edit.revision) {
      return t('tools.modelState.settingsChanged')
    }
    return null
  }
  function write(command: SaveCommand, edit: ModelEditContext): Promise<ModelSaveResult> {
    state.saving = true; state.storageError = ''; state.saved = ''
    return new Promise((resolve) => {
      // Install before send: a local bridge refusal may deliver its result synchronously.
      pending = { command, context: edit, resolve }
      try { connection.send(command) }
      catch { finish(fail(t('tools.modelState.couldNotSend'))) }
    })
  }
  function saveDefaults(value: ToolDefaults, edit: ModelEditContext): Promise<ModelSaveResult> {
    const reason = block(edit)
    if (reason !== null) return Promise.resolve(fail(reason))
    const document = decodeToolModelDocument({ version: 1, defaults: value, assignments: [] })
    if (document === null) return Promise.resolve(fail(t('tools.modelState.invalidDefaults')))
    if (state.storage === 'available' && panel.toolModels?.availability === 'available') {
      return write({ type: 'set_tool_model_defaults', machine_id: panel.toolModels.machine_id,
        base_revision: panel.toolModels.revision, defaults: document.defaults }, edit)
    }
    state.defaults = document.defaults
    state.storageError = ''
    if (state.storage === 'browser') {
      try { browserStorage().setItem(storageKey, JSON.stringify({ version: 1, defaults: document.defaults })) }
      catch { return Promise.resolve(fail(t('tools.modelState.browserStorageUnavailable'))) }
    }
    state.saved = state.storage === 'browser' ? t('tools.modelState.defaultsSavedBrowser') : t('tools.modelState.defaultsApplied')
    return Promise.resolve({ saved: true, reason: '' })
  }
  function saveAssignment(tool: Tool, family: ToolFamily | null, overrides: ToolOverrides,
    edit: ModelEditContext, holder?: ToolHolder | null): Promise<ModelSaveResult> {
    const reason = block(edit)
    if (reason !== null) return Promise.resolve(fail(reason))
    if (family === 'touch-probe' && !panel.simulated && state.storage !== 'available')
      return Promise.resolve(fail(t('tools.modelState.probeNeedsStorage')))
    const binding = toolModelBinding(tool)
    if (binding === null) return Promise.resolve(fail(tool.number <= 0
      ? t('tools.modelState.chooseNumbered')
      : t('tools.modelState.incomplete')))
    if (!panel.toolTable?.tools.some((row) => toolIdentity(row) === toolIdentity(tool)) ||
      !panel.simulated && panel.link !== 'live') {
      return Promise.resolve(fail(t('tools.modelState.tableRequired')))
    }
    const chosen = family === null ? undefined : holder === undefined ? state.assignments.get(tool.number)?.holder : holder ?? undefined
    const holderField = chosen === undefined ? {} : { holder: chosen }
    const document = decodeToolModelDocument({ version: 1, defaults: state.defaults,
      assignments: family === null ? [] : [{ tool: binding, family, overrides, ...holderField }] })
    if (document === null || family === null && Object.keys(overrides).length !== 0) {
      return Promise.resolve(fail(t('tools.modelState.invalidAssignment')))
    }
    if (state.storage === 'available' && panel.toolModels?.availability === 'available') {
      return write({ type: 'set_tool_model_assignment', machine_id: panel.toolModels.machine_id,
        base_revision: panel.toolModels.revision, tool: binding, family, overrides: { ...overrides }, ...holderField }, edit)
    }
    fixtureModels.delete(tool.number)
    if (family === null) state.assignments.delete(tool.number)
    else state.assignments.set(tool.number, { identity: toolIdentity(tool), family, overrides: { ...overrides }, ...holderField })
    state.storageError = ''
    state.saved = t('tools.modelState.applied')
    return Promise.resolve({ saved: true, reason: '' })
  }
  return { state, context, block, saveDefaults, saveAssignment,
    dispose: (): void => {
      for (const stop of stops) stop()
      if (pending !== null) finish(fail(t('tools.modelState.viewClosed')))
    } }
}
