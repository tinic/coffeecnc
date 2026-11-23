import { reactive, ref, watch, type Ref } from 'vue'
import type { CommandBody, Hello } from './protocol'
import type { Panel, PanelState } from './store'
import { decodeOperatorPreferences, defaultOperatorPreferences, withRecentProgram, type OperatorPreferences, type PanelTheme, type ProgramLayout } from './operator-settings'
import { decodeLanguage, t, type Language } from './i18n'
import { validMachineId } from './tool-model-settings'

const storageKey = 'coffeecnc.operator-settings.v1'
// The language is kept apart from the settings, as on the machine (panel-language.json): earlier
// versions refuse a settings document with a field they do not know.
const languageKey = 'coffeecnc.panel-language.v1'
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

interface StartupPresentation {
  theme: Ref<PanelTheme>
  layoutOverride?: ProgramLayout | null
  themeOverride?: PanelTheme | null
  language?: Ref<Language>
  languageOverride?: Language | null
}

type PresentationChange = Partial<Pick<OperatorPreferences, 'program_layout' | 'theme' | 'language' | 'recent_programs'>>

export interface OperatorSettingsController {
  state: SettingsState
  context: () => SettingsContext
  block: (edit: SettingsContext) => string | null
  save: (value: OperatorPreferences, edit: SettingsContext) => Promise<SettingsResult>
  chooseProgramLayout: (layout: ProgramLayout) => void
  rememberProgram: (path: string) => void
  chooseTheme: (theme: PanelTheme) => void
  chooseLanguage: (language: Language) => void
  dispose: () => void
}

/** One instance per page, so opening an editor never reapplies startup defaults. */
export function createOperatorSettingsState(panel: PanelState, connection: Pick<Panel, 'send' | 'onResult'>,
  browserStorage: () => Pick<Storage, 'getItem' | 'setItem'> = () => localStorage,
  presentation?: StartupPresentation): OperatorSettingsController {
  const state = reactive<SettingsState>({ storage: panel.simulated ? 'browser' : 'loading', reason: '',
    error: '', revision: null, saving: false, uncertain: false })
  if (panel.simulated) {
    try {
      const saved: unknown = JSON.parse(browserStorage().getItem(storageKey) ?? 'null')
      panel.preferences = decodeOperatorPreferences(saved) ?? defaultOperatorPreferences()
      panel.preferences.language = decodeLanguage(browserStorage().getItem(languageKey)) ?? 'en'
    } catch { /* Browser demo can still run with session defaults. */ }
  }
  const theme = presentation?.theme ?? ref<PanelTheme>('light')
  const layoutOverride = presentation?.layoutOverride ?? null
  const themeOverride = presentation?.themeOverride ?? null
  panel.programLayout = layoutOverride ?? panel.preferences.program_layout
  theme.value = themeOverride ?? panel.preferences.theme
  const language = presentation?.language ?? ref<Language>('en')
  const languageOverride = presentation?.languageOverride ?? null
  language.value = languageOverride ?? panel.preferences.language
  let initialLayout = panel.programLayout
  let layoutChosen = false
  let themeChosen = false
  let languageChosen = false
  let queuedPresentation: PresentationChange = {}
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
    if (!layoutChosen && panel.programLayout === initialLayout)
      panel.programLayout = layoutOverride ?? prefs.program_layout
    if (!themeChosen) theme.value = themeOverride ?? prefs.theme
    if (!languageChosen) language.value = languageOverride ?? prefs.language
    // A delayed first snapshot must not undo an operator's intervening edits.
    if (panel.unit === initialUnit) panel.unit = prefs.startup_units === 'machine' ? hello.machine_units : prefs.startup_units
    if (panel.jogVelocity === initialJog) panel.jogVelocity = Math.min(prefs.default_jog_velocity, hello.max_jog_velocity)
    panel.startupSpindleRpm = Math.min(prefs.default_spindle_rpm, hello.max_spindle_rpm ?? Infinity)
  }
  stops.push(watch(() => panel.hello, (hello) => {
    if (pending !== null) finish(fail(t('status.settingsState.draftUnconfirmed')))
    state.error = ''; state.reason = ''; state.uncertain = false; state.revision = null
    if (hello === null) { state.storage = 'loading'; return }
    if (machineId !== hello.machine_id) {
      if (machineId !== undefined) {
        // A choice queued while disconnected belongs to the previous machine.
        queuedPresentation = {}
        layoutChosen = false
        themeChosen = false
        languageChosen = false
        initialLayout = panel.programLayout
      }
      initialized = false
      machineId = hello.machine_id
      panel.startupSpindleRpm = 0
    }
    initialUnit = panel.unit; initialJog = panel.jogVelocity
    if (panel.simulated) { state.storage = 'browser'; applyStartup(); return }
    panel.preferences = defaultOperatorPreferences()
    panel.preferences.default_jog_velocity = Math.min(600, hello.max_jog_velocity)
    const capability: unknown = Reflect.get(hello, 'operator_settings_storage')
    if (capability === undefined) {
      state.storage = 'session_only'; state.reason = t('status.settingsState.cannotSave')
      applyStartup()
    } else if (capability !== 1 || !(hello.machine_id === null || validMachineId(hello.machine_id))) {
      state.storage = 'unavailable'; state.reason = t('status.settingsState.invalidIdentity')
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
      finish(fail(state.uncertain ? t('status.settingsState.noConfirmation')
        : result.reason || t('status.settingsState.saveFailed')))
    } else if (panel.hello !== pending.context.hello || state.storage !== 'available' ||
      state.revision === null || state.revision <= pending.command.base_revision) {
      state.uncertain = true
      finish(fail(t('status.settingsState.noUpdatedSettings')))
    } else { state.error = ''; finish({ saved: true, reason: '' }) }
  }))
  function context(): SettingsContext { return { hello: panel.hello, revision: state.revision } }
  function block(edit: SettingsContext): string | null {
    if (panel.hello !== edit.hello) return t('status.settingsState.connectionChanged')
    if (state.saving) return t('status.settingsState.saving')
    if (state.uncertain) return state.error
    if (state.storage === 'loading') return t('status.settingsState.waiting')
    if (state.storage === 'unavailable') return state.reason || t('status.settingsState.storageUnavailable')
    if (state.storage === 'available' && edit.revision !== state.revision) return t('status.settingsState.savedChanged')
    return null
  }
  function save(value: OperatorPreferences, edit: SettingsContext): Promise<SettingsResult> {
    const reason = block(edit)
    if (reason !== null) return Promise.resolve(fail(reason))
    const document = decodeOperatorPreferences(value)
    if (document === null) return Promise.resolve(fail(t('status.settingsState.checkValues')))
    // The decoder drops a damaged list; a save must never drop the operator's positions that way.
    if (document.saved_positions.length !== value.saved_positions.length) {
      return Promise.resolve(fail(t('status.settingsState.checkPositions')))
    }
    if (document.default_jog_velocity > (panel.hello?.max_jog_velocity ?? Infinity) ||
      document.default_spindle_rpm > (panel.hello?.max_spindle_rpm ?? Infinity)) {
      return Promise.resolve(fail(t('status.settingsState.speedLimit')))
    }
    document.enabled_wcs.sort((a, b) => a - b)
    if (state.storage === 'available' && panel.operatorSettings?.availability === 'available') {
      const command: SaveCommand = { type: 'set_operator_settings', machine_id: panel.operatorSettings.machine_id,
        base_revision: panel.operatorSettings.revision, document }
      state.saving = true; state.error = ''
      return new Promise((resolve) => {
        pending = { command, context: edit, resolve }
        try { connection.send(command) } catch { finish(fail(t('status.settingsState.sendFailed'))) }
      })
    }
    if (state.storage === 'browser') {
      const { language: chosen, ...stored } = document
      try {
        if (chosen !== panel.preferences.language) browserStorage().setItem(languageKey, chosen)
        browserStorage().setItem(storageKey, JSON.stringify(stored))
      }
      catch { return Promise.resolve(fail(t('status.settingsState.browserStorage'))) }
    }
    panel.preferences = document
    state.error = ''
    return Promise.resolve({ saved: true, reason: '' })
  }
  function flushPresentation(): void {
    if (state.storage === 'loading' || state.saving || state.uncertain) return
    const patch = queuedPresentation
    queuedPresentation = {}
    if (state.storage === 'unavailable') return
    const { program_layout: layout, theme: storedTheme, language: storedLanguage, recent_programs: recent } = panel.preferences
    if ((patch.program_layout ?? layout) === layout && (patch.theme ?? storedTheme) === storedTheme &&
        (patch.language ?? storedLanguage) === storedLanguage &&
        (patch.recent_programs ?? recent).join('\n') === recent.join('\n')) return
    // Merge into the latest confirmed document so a queued choice cannot undo another save.
    void save({ ...panel.preferences, ...patch }, context())
  }
  function chooseProgramLayout(layout: ProgramLayout): void {
    layoutChosen = true
    panel.programLayout = layout
    queuedPresentation.program_layout = layout
    flushPresentation()
  }
  /** Puts the program the machine has loaded first in the recent list. */
  function rememberProgram(path: string): void {
    queuedPresentation.recent_programs = withRecentProgram(queuedPresentation.recent_programs ?? panel.preferences.recent_programs, path)
    flushPresentation()
  }
  function chooseTheme(value: PanelTheme): void {
    themeChosen = true
    theme.value = value
    queuedPresentation.theme = value
    flushPresentation()
  }
  function chooseLanguage(value: Language): void {
    languageChosen = true
    language.value = value
    queuedPresentation.language = value
    flushPresentation()
  }
  stops.push(watch([
    (): SettingsState['storage'] => state.storage,
    (): boolean => state.saving,
    (): boolean => state.uncertain,
  ], flushPresentation, { flush: 'post' }))

  stops.push(watch(() => panel.status?.program.file ?? null, (file) => {
    if (file !== null && file !== panel.preferences.recent_programs[0]) rememberProgram(file)
  }))

  return { state, context, block, save, chooseProgramLayout, chooseTheme, chooseLanguage, rememberProgram, dispose: (): void => {
    for (const stop of stops) stop()
    if (pending !== null) finish(fail(t('status.settingsState.closed')))
  } }
}
