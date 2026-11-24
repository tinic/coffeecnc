// The panel's store: the latest of everything the machine reports, the commands still waiting
// for their result, and what the operator has been told. The machine is the source of truth; the
// only state that belongs to the panel is presentation (workspace, display unit).

import { reactive } from 'vue'

import type { Transport } from './bridge'
import type {
  AlarmLog,
  BackupWritten,
  CommandBody,
  CommandResult,
  MessageKey,
  Diagnostics,
  Directory,
  FromMachine,
  Hello,
  LengthUnit,
  OffsetHistory,
  Preview,
  ProgramText,
  RestoreSummary,
  Status,
  ToolTable,
  ToolModels,
  WorkOffsets,
} from './protocol'
import { protocolVersion } from './protocol'
import { defaultOperatorPreferences, decodeOperatorSettings, type OperatorPreferences, type OperatorSettings, type ProgramLayout } from './operator-settings'
import { decodeToolModels, validMachineId } from './tool-model-settings'
import type { LinkFault } from './sim/machine'
import { baseName, wcsName } from './units'

export type Tab = 'manual' | 'program' | 'tools' | 'probe'
export const tabs: readonly Tab[] = ['manual', 'program', 'tools', 'probe']
export type { ProgramLayout } from './operator-settings'

/** Status older than this is not live, whatever its `link` says. */
export const staleAfterMs = 1000
/** A command without a result for this long is reported to the operator. */
export const answerTimeoutMs = 2000
/**
 * A restart answers when its approach is done: the machine stops the spindle (up to 15 s), lifts
 * Z and moves to X Y (45 s each), feeds Z down (up to 180 s) and sends about fifteen blocks of 4 s
 * each (NmlPort::start_restart). The confirmed tool change of a restart answers after the same
 * approach. Until then the dialog keeps the request, and Stop still ends it.
 */
export const restartTimeoutMs = 420_000
const programFileTimeoutMs = 60_000
const watchdogMs = 200
const maxLogEntries = 500
const maxMdiEntries = 50
const collapseWindowMs = 3000

/**
 * What the panel can say about the link: `silent` is the panel's own finding that no status has
 * arrived for a second; `stale` and `down` are reported by the executable. `incompatible` is a
 * hello protocol mismatch; incoming snapshots cannot be interpreted as controller state.
 */
export type LinkView = 'connecting' | 'live' | 'silent' | 'stale' | 'down' | 'incompatible'

export interface LogEntry {
  id: number
  level: 'error' | 'info'
  text: string
  localization?: MessageKey
  /** What was asked for, when the entry is the result of a command. */
  context: string
  /** Milliseconds since the Unix epoch, of the last occurrence. */
  time: number
  /** Identical entries in quick succession are counted, not repeated. */
  count: number
}

export interface MdiEntry {
  id: number
  text: string
  outcome: 'pending' | CommandResult['outcome']
  reason: string
  localization?: MessageKey
}

export interface PanelState {
  simulated: boolean
  hello: Hello | null
  status: Status | null
  link: LinkView
  /** Milliseconds since the last status arrived; meaningful when `link` is `silent`. */
  silentFor: number
  toolTable: ToolTable | null
  /** Null until a valid snapshot arrives for the current hello. Never implies an empty file. */
  toolModels: (ToolModels & { localization?: MessageKey }) | null
  operatorSettings: OperatorSettings | null
  preferences: OperatorPreferences
  /** Startup preset only; never a controller command. */
  startupSpindleRpm: number
  workOffsets: WorkOffsets | null
  /** Null until the executable sends one; a peer without a history never does. */
  offsetHistory: OffsetHistory | null
  /** What was told before this page was loaded; null until the executable sends it. */
  alarmLog: AlarmLog | null
  /** The last HAL reading, while the diagnostics view watches; null before the first. */
  diagnostics: Diagnostics | null
  program: ProgramText | null
  preview: Preview | null
  directory: Directory | null
  /** The last backup written, and the last restore checked, for the backup dialog. */
  backupWritten: BackupWritten | null
  restoreSummary: RestoreSummary | null
  log: LogEntry[]
  /** The log entry shown in the status bar, until dismissed. */
  notice: LogEntry | null
  mdi: MdiEntry[]
  /** Changes before a panel Stop, E-stop, or machine-off request reaches the transport. */
  motionStopRequest: number
  tab: Tab
  unit: LengthUnit
  programLayout: ProgramLayout
  droDetails: boolean
  /** Jog step, mm, or null for continuous jogging. */
  jogIncrement: number | null
  /** mm/min. */
  jogVelocity: number
  simulatorFault: LinkFault
}

export interface PanelTimers {
  setInterval: (callback: () => void, ms: number) => number
  clearInterval: (handle: number) => void
}

export interface PanelOptions {
  transport: Transport
  timers?: PanelTimers
  now?: () => number
}

export type ResultListener = (command: CommandBody, result: CommandResult) => void

export interface Panel {
  readonly state: PanelState
  /** Sends a command and returns its id. */
  send: (command: CommandBody) => number
  start: () => void
  stop: () => void
  /** Reports command results, including local timeouts; returns a listener removal function. */
  onResult: (listener: ResultListener) => () => void
  dismissNotice: () => void
  setSimulatorFault: (fault: LinkFault) => void
}

/** Reconnect and stop attempts remain available; none imply that a mismatched peer accepted them. */
function allowedWhileIncompatible(command: CommandBody): boolean {
  return command.type === 'ready' || command.type === 'abort' || command.type === 'jog_stop_all' ||
    command.type === 'jog' && !command.held || command.type === 'set_machine' && command.state === 'estop'
}

/** A few words naming a command, for the log. */
export function describeCommand(command: CommandBody): string {
  switch (command.type) {
    case 'ready':
      return 'Connect'
    case 'quit':
      return 'Quit CoffeeCNC'
    case 'set_machine':
      return command.state === 'estop' ? 'E-stop' : command.state === 'on' ? 'Machine on' : 'Machine off'
    case 'home':
      return command.axis === 'all' ? 'Home all' : `Home ${command.axis.toUpperCase()}`
    case 'jog':
    case 'jog_step':
      return `Jog ${command.axis.toUpperCase()}`
    case 'jog_paused_z':
      return command.distance > 0 ? 'Lift paused Z' : 'Lower paused Z'
    case 'jog_stop_all':
      return 'Stop jog'
    case 'mdi':
      return `MDI ${command.text}`
    case 'list_directory':
      return 'List directory'
    case 'manage_program_file':
      return 'Manage program files'
    case 'open_program':
      return `Open ${command.path}`
    case 'run':
      return command.breakpoint ? `Run to line ${String(command.breakpoint.line)}` : 'Cycle start'
    case 'pause':
      return 'Pause'
    case 'resume':
      return 'Resume'
    case 'step':
      return 'Single step'
    case 'abort':
      return 'Stop'
    case 'set_override':
      return `Set ${command.which} override`
    case 'set_spindle':
      return command.direction === 'stopped' ? 'Spindle stop' : `Spindle ${command.direction}`
    case 'set_coolant':
      return 'Coolant'
    case 'set_program_option':
      return command.option === 'optional_stop' ? 'Optional stop' : 'Block delete'
    case 'select_wcs':
      return `Select ${wcsName(command.wcs)}`
    case 'set_work_offset':
      return `Set ${wcsName(command.wcs)} XYZ origins`
    case 'set_work_rotation':
      return `Set ${wcsName(command.wcs)} XY rotation`
    case 'clear_g92':
      return 'Clear G92/G52 shift'
    case 'plan_restart':
      return `Plan restart at line ${String(command.line)}`
    case 'start_restart':
      return 'Restart program'
    case 'edit_program':
      return `Edit line ${String(command.line)}`
    case 'save_program':
      return `Save ${baseName(command.path)}`
    case 'touch_off':
      return command.origin === 'zero' ? `Zero ${command.axis.toUpperCase()}`
        : command.origin === 'half' ? `Half ${command.axis.toUpperCase()}`
          : `Touch off ${command.axis.toUpperCase()}`
    case 'restore_offset':
      return 'Restore from offset history'
    case 'tool_touch_off':
      return 'Tool length touch-off'
    case 'fixed_setter':
      if (command.action === 'check') return 'Check tool length'
      if (command.action === 'angle') return 'Measure edge angle'
      if (command.action === 'apply_angle') return 'Apply work rotation'
      return command.action === 'material' ? 'Material touch-off' : 'Fixed tool setter'
    case 'set_tool':
      return `Edit tool ${String(command.tool.number)}`
    case 'remove_tool':
      return `Remove tool ${String(command.number)}`
    case 'tool_changed':
      return command.measure_request === undefined ? 'Confirm tool change' : 'Measure tool during change'
    case 'set_tool_model_defaults':
      return 'Save tool model defaults'
    case 'set_tool_model_assignment':
      return 'Save tool preview model'
    case 'set_operator_settings':
      return 'Save settings'
    case 'backup':
      return 'Back up machine data'
    case 'check_restore':
      return 'Check backup'
    case 'restore':
      return 'Restore backup'
    case 'watch_diagnostics':
      return command.on ? 'Open diagnostics' : 'Close diagnostics'
    case 'launch_hal_tool':
      return `Start ${command.tool}`
  }
}

function outcomeWords(outcome: CommandResult['outcome']): string {
  switch (outcome) {
    case 'accepted':
      return 'accepted'
    case 'rejected':
      return 'not possible now'
    case 'failed':
      return 'LinuxCNC reported an error'
    case 'timed_out':
      return 'LinuxCNC did not answer'
  }
}

export function createPanel(options: PanelOptions): Panel {
  const transport = options.transport
  const now = options.now ?? ((): number => Date.now())
  const timers: PanelTimers = options.timers ?? {
    setInterval: (callback, ms): number => window.setInterval(callback, ms),
    clearInterval: (handle): void => {
      window.clearInterval(handle)
    },
  }

  const state = reactive<PanelState>({
    simulated: transport.simulated,
    hello: null,
    status: null,
    link: 'connecting',
    silentFor: 0,
    toolTable: null,
    toolModels: null,
    operatorSettings: null,
    preferences: defaultOperatorPreferences(),
    startupSpindleRpm: 0,
    workOffsets: null,
    offsetHistory: null,
    alarmLog: null,
    diagnostics: null,
    program: null,
    preview: null,
    directory: null,
    backupWritten: null,
    restoreSummary: null,
    log: [],
    notice: null,
    mdi: [],
    motionStopRequest: 0,
    tab: 'manual',
    unit: 'mm',
    programLayout: 'path',
    droDetails: true,
    jogIncrement: null,
    jogVelocity: 600,
    simulatorFault: transport.simulator?.fault() ?? 'none',
  })

  const pending = new Map<number, { command: CommandBody; deadline: number }>()
  const resultListeners = new Set<ResultListener>()
  let nextId = 1
  let nextLogId = 1
  let lastStatusAt = 0
  let protocolError: string | null = null
  let watchdog: number | null = null
  // An accepted restart that waits for its tool change: the confirmation runs the approach.
  let restartToolChange = false

  function log(level: 'error' | 'info', text: string, context: string, time: number, announce = true, localization?: MessageKey): void {
    const last = state.log.at(-1)
    if (last?.level === level && last.text === text && last.context === context && time - last.time < collapseWindowMs &&
      JSON.stringify(last.localization) === JSON.stringify(localization)) {
      last.count += 1
      last.time = time
      if (announce && (level === 'error' || state.notice?.level !== 'error')) state.notice = last
      return
    }
    state.log.push({ id: nextLogId, level, text, context, time, count: 1, ...(localization ? { localization } : {}) })
    nextLogId += 1
    if (state.log.length > maxLogEntries) {
      state.log.splice(0, state.log.length - maxLogEntries)
    }
    // An informational message must not hide an error before the operator dismisses it.
    if (announce && (level === 'error' || state.notice?.level !== 'error')) state.notice = state.log.at(-1) ?? null
  }

  function updateLink(): void {
    if (protocolError !== null) {
      state.link = 'incompatible'
      state.silentFor = 0
      return
    }
    const status = state.status
    if (status === null) {
      state.link = 'connecting'
      return
    }
    const age = now() - lastStatusAt
    if (age > staleAfterMs) {
      state.link = 'silent'
      state.silentFor = age
    } else {
      state.link = status.link
      state.silentFor = 0
    }
  }

  function result(message: CommandResult): void {
    const entry = pending.get(message.id)
    if (entry === undefined) {
      return
    }
    pending.delete(message.id)
    const command = entry.command
    const localization = message.outcome === 'rejected' ? message.refusal ?? message.localization : message.localization
    if (command.type === 'start_restart') restartToolChange = message.outcome === 'accepted'
    if (command.type === 'mdi') {
      const mdi = state.mdi.find((candidate) => candidate.id === message.id)
      if (mdi !== undefined) {
        mdi.outcome = message.outcome
        mdi.reason = message.reason
        if (localization !== undefined) mdi.localization = localization
      }
    }
    if (message.outcome !== 'accepted') {
      const reason = message.reason === '' ? outcomeWords(message.outcome) : message.reason
      // These results are shown where the operator asked; the log still keeps them.
      const localResult = command.type === 'plan_restart' || command.type === 'fixed_setter' &&
        (command.action === 'material' && state.tab === 'probe' ||
         (command.action === 'touch_off' || command.action === 'check') && state.tab === 'tools')
      log('error', reason, describeCommand(command), now(), !localResult, localization)
    }
    for (const listener of resultListeners) {
      listener(command, message)
    }
  }

  function receive(message: FromMachine): void {
    if (protocolError !== null && message.type !== 'hello') return
    switch (message.type) {
      case 'hello':
        restartToolChange = false
        state.operatorSettings = null
        state.toolModels = null
        state.toolTable = null
        state.offsetHistory = null
        state.alarmLog = null
        state.diagnostics = null
        if (message.protocol !== protocolVersion) {
          protocolError = `Protocol mismatch: the machine speaks version ${String(message.protocol)}, this panel version ${String(protocolVersion)}`
          // These snapshots belong to a protocol we can no longer interpret. Clearing hello
          // also ends held panel jogs and invalidates page-local tool assignments.
          state.hello = null
          state.status = null
          state.toolTable = null
          state.workOffsets = null
          state.program = null
          state.preview = null
          state.directory = null
          updateLink()
          log('error', protocolError, '', now())
          break
        }
        protocolError = null
        if (state.hello === null) {
          state.unit = message.machine_units
        }
        state.jogVelocity = Math.min(state.jogVelocity, message.max_jog_velocity)
        state.hello = message
        updateLink()
        break
      case 'status':
        state.status = message
        // A restart without a tool change answers once the program runs.
        if (message.mode === 'auto') restartToolChange = false
        // A live status identifies the loaded program. Down/stale placeholders must not erase
        // text being read as history; snapshots may also arrive before their matching status.
        if (message.link === 'live' && (message.program.file === null ||
          state.program?.file !== message.program.file || state.program.revision !== message.program.revision)) {
          state.program = null
        }
        lastStatusAt = now()
        updateLink()
        break
      case 'tool_table':
        state.toolTable = message
        break
      case 'tool_models': {
        // The native bridge already binds messages to a page. Bind persistence to its current
        // hello too; an old machine's snapshot must never provide a revision for this machine.
        if (state.hello?.tool_model_storage !== 1 || message.machine_id !== state.hello.machine_id) break
        const decoded = decodeToolModels(message)
        if (decoded === null) {
          const reason = 'Invalid tool model settings received. Saving is unavailable.'
          state.toolModels = { type: 'tool_models', machine_id: validMachineId(message.machine_id) ? message.machine_id : null,
            revision: 0, availability: 'unavailable', reason, document: null }
          log('error', reason, 'Tool models', now())
        } else if (state.toolModels === null || decoded.availability !== 'available' ||
          state.toolModels.availability !== 'available' || decoded.revision >= state.toolModels.revision) {
          state.toolModels = decoded
        }
        break
      }
      case 'tool_models_reason': {
        const snapshot = state.toolModels
        if (snapshot !== null && snapshot.machine_id === message.machine_id &&
          snapshot.revision === message.revision && snapshot.reason === message.reason) {
          state.toolModels = { ...snapshot, localization: message.localization }
        }
        break
      }
      case 'operator_settings': {
        if (state.hello?.operator_settings_storage !== 1 || message.machine_id !== state.hello.machine_id) break
        const decoded = decodeOperatorSettings(message)
        if (decoded === null) {
          const reason = 'Invalid operator settings received. Saving is unavailable.'
          state.operatorSettings = { type: 'operator_settings', machine_id: validMachineId(message.machine_id) ? message.machine_id : null,
            revision: 0, availability: 'unavailable', reason, document: null }
          log('error', reason, 'Settings', now())
        } else if (state.operatorSettings === null || decoded.availability !== 'available' ||
          state.operatorSettings.availability !== 'available' || decoded.revision >= state.operatorSettings.revision) {
          state.operatorSettings = decoded
        }
        break
      }
      case 'work_offsets':
        state.workOffsets = message
        break
      case 'offset_history':
        state.offsetHistory = message
        break
      case 'alarm_log':
        state.alarmLog = message
        break
      case 'diagnostics':
        state.diagnostics = message
        break
      case 'program':
        state.program = message
        break
      case 'preview':
        state.preview = message
        break
      case 'directory':
        state.directory = message
        break
      case 'backup_written':
        state.backupWritten = message
        break
      case 'restore_summary':
        state.restoreSummary = message
        break
      case 'message':
        log(message.level, message.text, '', message.time, true, message.localization)
        break
      case 'result':
        result(message)
        break
    }
  }

  function checkPending(): void {
    const time = now()
    for (const [id, entry] of pending) {
      if (time > entry.deadline) {
        // A local deadline must also end held controls through their result listeners. It
        // does not prove the controller cancelled the command; a release is still necessary.
        result({ type: 'result', id, outcome: 'timed_out', reason: 'No answer from the machine' })
      }
    }
  }

  transport.listen(receive)

  return {
    state,
    send: (command): number => {
      const id = nextId
      nextId += 1
      const pausedZMove = command.type === 'jog_paused_z' ||
        command.type === 'resume' && (state.status?.paused_z?.displaced === true ||
          state.status?.paused_z?.spindle === 'restore')
      const restartMove = command.type === 'start_restart' ||
        command.type === 'tool_changed' && restartToolChange
      if (command.type === 'tool_changed' || command.type === 'abort') restartToolChange = false
      if (command.type === 'abort' || command.type === 'set_machine' && command.state !== 'on') {
        state.motionStopRequest += 1
      }
      const measureChange = command.type === 'tool_changed' && command.measure_request !== undefined
      const timeout = measureChange || command.type === 'fixed_setter' && command.action !== 'status'
        ? 1_600_000 : pausedZMove ? 160_000 : restartMove ? restartTimeoutMs
          : command.type === 'manage_program_file' ? programFileTimeoutMs : answerTimeoutMs
      pending.set(id, { command, deadline: now() + timeout })
      if (command.type === 'mdi') {
        state.mdi.push({ id, text: command.text, outcome: 'pending', reason: '' })
        if (state.mdi.length > maxMdiEntries) {
          state.mdi.splice(0, state.mdi.length - maxMdiEntries)
        }
      }
      if (protocolError !== null && !allowedWhileIncompatible(command)) {
        result({ type: 'result', id, outcome: 'rejected', reason: protocolError })
        return id
      }
      transport.send({ ...command, id })
      return id
    },
    start: (): void => {
      watchdog ??= timers.setInterval(() => {
        updateLink()
        checkPending()
      }, watchdogMs)
    },
    stop: (): void => {
      if (watchdog !== null) {
        timers.clearInterval(watchdog)
        watchdog = null
      }
    },
    onResult: (listener): (() => void) => {
      resultListeners.add(listener)
      return () => {
        resultListeners.delete(listener)
      }
    },
    dismissNotice: (): void => {
      state.notice = null
    },
    setSimulatorFault: (fault): void => {
      transport.simulator?.setFault(fault)
      state.simulatorFault = transport.simulator?.fault() ?? 'none'
    },
  }
}
