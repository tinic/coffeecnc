import { reactive, watch } from 'vue'
import type { CommandBody, Hello } from './protocol'
import type { Panel, PanelState } from './store'
import { decodeOperatorPreferences, defaultOperatorPreferences, type OperatorPreferences } from './operator-settings'
import { validMachineId } from './tool-model-settings'

const storageKey = 'coffeecnc.operator-settings.v1'
type SaveCommand = Extract<CommandBody, { type: 'set_operator_settings' }>
export interface SettingsContext { hello: Hello | null; revision: number | null }
export interface SettingsResult { saved: boolean; reason: string }
export interface SettingsState {
  storage: 'loading' | 'browser' | 'available' | 'session_only' | 'unavailable'
  reason: string
  error: string
  revision: number | null
  saving: boolean
  uncertain: boolean
}

export interface OperatorSettingsController {
  state: SettingsState
  context: () => SettingsContext
  block: (edit: SettingsContext) => string | null
  save: (value: OperatorPreferences, edit: SettingsContext) => Promise<SettingsResult>
  dispose: () => void
}

/** One instance per page, so opening an editor never reapplies startup defaults. */
export function createOperatorSettingsState(panel: PanelState, connection: Pick<Panel, 'send' | 'onResult'>,
  browserStorage: () => Pick<Storage, 'getItem' | 'setItem'> = () => localStorage): OperatorSettingsController {
  const state = reactive<SettingsState>({ storage: panel.simulated ? 'browser' : 'loading', reason: '',
    error: '', revision: null, saving: false, uncertain: false })
  if (panel.simulated) {
    try {
      const saved: unknown = JSON.parse(browserStorage().getItem(storageKey) ?? 'null')
      panel.preferences = decodeOperatorPreferences(saved) ?? defaultOperatorPreferences()
    } catch { /* Browser demo can still run with session defaults. */ }
  }
  let initialized = false
  let machineId: string | null | undefined
  let initialUnit = panel.unit
  let initialJog = panel.jogVelocity
  let pending: { command: SaveCommand; context: SettingsContext; resolve: (value: SettingsResult) => void } | null = null
  const stops: (() => void)[] = []
  function fail(reason: string): SettingsResult { state.error = reason; return { saved: false, reason } }
  function finish(value: SettingsResult): void {
    const request = pending; pending = null; state.saving = false; request?.resolve(value)
  }
  function applyStartup(): void {
    const hello = panel.hello
    if (initialized || hello === null) return
    initialized = true
    const prefs = panel.preferences
    // A delayed first snapshot must not undo an operator's intervening edits.
    if (panel.unit === initialUnit) panel.unit = prefs.startup_units === 'machine' ? hello.machine_units : prefs.startup_units
    if (panel.jogVelocity === initialJog) panel.jogVelocity = Math.min(prefs.default_jog_velocity, hello.max_jog_velocity)
    panel.startupSpindleRpm = Math.min(prefs.default_spindle_rpm, hello.max_spindle_rpm ?? Infinity)
  }
  stops.push(watch(() => panel.hello, (hello) => {
    if (pending !== null) finish(fail('The machine connection changed. This draft was not confirmed saved.'))
    state.error = ''; state.reason = ''; state.uncertain = false; state.revision = null
    if (hello === null) { state.storage = 'loading'; return }
    if (machineId !== hello.machine_id) { initialized = false; machineId = hello.machine_id; panel.startupSpindleRpm = 0 }
    initialUnit = panel.unit; initialJog = panel.jogVelocity
    if (panel.simulated) { state.storage = 'browser'; applyStartup(); return }
    panel.preferences = defaultOperatorPreferences()
    panel.preferences.default_jog_velocity = Math.min(600, hello.max_jog_velocity)
    const capability: unknown = Reflect.get(hello, 'operator_settings_storage')
    if (capability === undefined) {
      state.storage = 'session_only'; state.reason = 'This backend cannot save settings. Changes last until CoffeeCNC closes.'
      applyStartup()
    } else if (capability !== 1 || !(hello.machine_id === null || validMachineId(hello.machine_id))) {
      state.storage = 'unavailable'; state.reason = 'The machine did not provide a valid settings identity.'
      applyStartup()
    } else state.storage = 'loading'
  }, { immediate: true, flush: 'sync' }))
  stops.push(watch(() => panel.operatorSettings, (snapshot) => {
    if (panel.simulated || panel.hello?.operator_settings_storage !== 1) return
    if (snapshot === null) { state.storage = 'loading'; state.revision = null; return }
    state.storage = snapshot.availability; state.reason = snapshot.reason; state.uncertain = false
    state.revision = snapshot.availability === 'available' ? snapshot.revision : null
    if (snapshot.availability === 'available') panel.preferences = snapshot.document
    applyStartup()
  }, { immediate: true, flush: 'sync' }))
  stops.push(connection.onResult((command, result) => {
    if (command !== pending?.command) return
    if (result.outcome !== 'accepted') {
      state.uncertain = result.outcome === 'timed_out'
      finish(fail(state.uncertain ? 'No save confirmation. The write may still finish; wait for refreshed settings before retrying.'
        : result.reason || 'Settings could not be saved.'))
    } else if (panel.hello !== pending.context.hello || state.storage !== 'available' ||
      state.revision === null || state.revision <= pending.command.base_revision) {
      state.uncertain = true
      finish(fail('The save result arrived without updated settings. Reload CoffeeCNC to verify the saved values.'))
    } else { state.error = ''; finish({ saved: true, reason: '' }) }
  }))
  function context(): SettingsContext { return { hello: panel.hello, revision: state.revision } }
  function block(edit: SettingsContext): string | null {
    if (panel.hello !== edit.hello) return 'The connection changed. Close Settings and reopen it.'
    if (state.saving) return 'Saving settings…'
    if (state.uncertain) return state.error
    if (state.storage === 'loading') return 'Waiting for saved settings.'
    if (state.storage === 'unavailable') return state.reason || 'Settings storage is unavailable.'
    if (state.storage === 'available' && edit.revision !== state.revision) return 'Saved settings changed. Close Settings and reopen it.'
    return null
  }
  function save(value: OperatorPreferences, edit: SettingsContext): Promise<SettingsResult> {
    const reason = block(edit)
    if (reason !== null) return Promise.resolve(fail(reason))
    const document = decodeOperatorPreferences(value)
    if (document === null) return Promise.resolve(fail('Check the units, work systems and speeds.'))
    if (document.default_jog_velocity > (panel.hello?.max_jog_velocity ?? Infinity) ||
      document.default_spindle_rpm > (panel.hello?.max_spindle_rpm ?? Infinity)) {
      return Promise.resolve(fail('A default speed exceeds the configured machine limit.'))
    }
    document.enabled_wcs.sort((a, b) => a - b)
    if (state.storage === 'available' && panel.operatorSettings?.availability === 'available') {
      const command: SaveCommand = { type: 'set_operator_settings', machine_id: panel.operatorSettings.machine_id,
        base_revision: panel.operatorSettings.revision, document }
      state.saving = true; state.error = ''
      return new Promise((resolve) => {
        pending = { command, context: edit, resolve }
        try { connection.send(command) } catch { finish(fail('The settings save could not be sent.')) }
      })
    }
    if (state.storage === 'browser') {
      try { browserStorage().setItem(storageKey, JSON.stringify(document)) }
      catch { return Promise.resolve(fail('Browser storage is unavailable. Settings were not saved.')) }
    }
    panel.preferences = document
    state.error = ''
    return Promise.resolve({ saved: true, reason: '' })
  }
  return { state, context, block, save, dispose: (): void => {
    for (const stop of stops) stop()
    if (pending !== null) finish(fail('CoffeeCNC closed before save confirmation.'))
  } }
}

