// Messages between the operator panel and the coffeecnc executable.
//
// The panel shows state and requests actions; the executable owns the LinuxCNC connection and
// decides whether an action is valid. Field names match the C++ structs one to one.
//
// Units: lengths in millimetres, feeds in mm/min, spindle speed in rpm, overrides as factors
// (1.0 is 100 %). Conversion to inches happens only for display.
//
// The panel never sets LinuxCNC's task mode. A command implies the mode it needs and the
// executable switches mode when the interpreter is idle, or rejects the command.

export const protocolVersion = 1

export type AxisLetter = 'x' | 'y' | 'z'
export type Vec3 = [number, number, number]

/**
 * Whether the status comes from a running LinuxCNC task. A frozen executable cannot report its
 * own silence: the panel also treats the status as stale when none has arrived for 1 s.
 */
export type Link =
  | 'live' // task wrote this status within the last second
  | 'stale' // the last status task wrote; task has stopped writing
  | 'down' // no connection; every other field is a placeholder

/** `off` is LinuxCNC's ESTOP_RESET: E-stop cleared, machine power off. */
export type MachineState = 'estop' | 'off' | 'on'
export type TaskMode = 'manual' | 'mdi' | 'auto'
export type InterpState = 'idle' | 'reading' | 'paused' | 'waiting'
export type LengthUnit = 'mm' | 'inch'
export type SpindleDirection = 'stopped' | 'forward' | 'reverse'
export type FeedMode = 'per_minute' | 'inverse_time' | 'per_revolution'

// ---- machine to panel -----------------------------------------------------------------------

/** Sent once after the panel's `ready`: what does not change while the machine runs. */
export interface Hello {
  type: 'hello'
  protocol: number
  machine_name: string
  /** True when no LinuxCNC is behind this session (browser simulator, or `--demo`). */
  simulated: boolean
  axes: AxisLetter[]
  /** The machine's native unit ([TRAJ] LINEAR_UNITS); the default display unit. */
  machine_units: LengthUnit
  /** Jog increments offered, mm ([DISPLAY] INCREMENTS). */
  jog_increments: number[]
  /** Fastest jog, mm/min ([DISPLAY] MAX_LINEAR_VELOCITY). */
  max_jog_velocity: number
  /** Optional v1 addition, mm/min. When present, includes every configured axis. */
  axis_max_jog_velocity?: Partial<Record<AxisLetter, number>>
  /** Whether MDI/program execution requires homing. Absent means true. */
  homing_required?: boolean
  max_feed_override: number
  min_spindle_override: number
  max_spindle_override: number
}

export interface AxisStatus {
  axis: AxisLetter
  homed: boolean
  homing: boolean
  /** Actual position in machine coordinates. */
  machine: number
  /** Actual position in the active work coordinate system, with G92 and tool offset applied. */
  work: number
  /** Distance to go in the current move. */
  dtg: number
  min_limit: number
  max_limit: number
  /** A limit switch is tripped. */
  on_hard_limit: boolean
}

export interface Offsets {
  /** Active work coordinate system: 1 is G54 … 9 is G59.3. */
  wcs: number
  g5x: Vec3
  g92: Vec3
  /** Tool offset in effect (zero unless G43 is active). */
  tool: Vec3
  /** XY rotation of the active system, degrees (G10 L2 R). */
  rotation_xy: number
}

export interface MotionStatus {
  /** Current velocity along the path, mm/min. */
  velocity: number
  /** G94 / G93 / G95. */
  feed_mode: FeedMode
  /** The F word in effect, in the unit `feed_mode` gives: mm/min, 1/min, or mm/rev. */
  programmed_feed: number
  feed_override: number
  rapid_override: number
  in_position: boolean
}

/** Commanded spindle state plus optional, explicitly configured measurement signals. */
export interface SpindleStatus {
  direction: SpindleDirection
  /** The S word in effect, before override. */
  commanded_rpm: number
  override: number
  /** Measured speed magnitude in rpm, not derived from commanded speed. Null/absent = unavailable. */
  actual_rpm?: number | null
  /** Configured feedback signal; null/absent is unknown, not false. */
  at_speed?: boolean | null
}

export interface ProgramStatus {
  /** Path of the loaded program on the machine, or null. */
  file: string | null
  /** Changes whenever a program is loaded or reloaded; matches `ProgramText.revision`. */
  revision: number
  /** Line the interpreter is executing (1-based; 0 when none). Reads ahead of motion. */
  current_line: number
  /** Line the motion in progress came from (1-based; 0 when none). */
  motion_line: number
  optional_stop: boolean
  block_delete: boolean
}

/** Why a running program is not moving. The panel shows this instead of guessing. */
export type Wait =
  | { kind: 'none' }
  /** Paused: by the operator (feed hold) or by the program (M0, M1, M60). */
  | { kind: 'paused' }
  /** LinuxCNC asks for a manual tool change and waits for `tool_changed`. */
  | { kind: 'tool_change'; tool: number }
  | { kind: 'dwell'; seconds_left: number }

export interface Status {
  type: 'status'
  /** Increases by one with every status sent. */
  seq: number
  link: Link
  machine: MachineState
  mode: TaskMode
  interp: InterpState
  axes: AxisStatus[]
  offsets: Offsets
  motion: MotionStatus
  spindle: SpindleStatus
  coolant: { flood: boolean; mist: boolean }
  /** Configured probe input: true means tripped. Null/absent = unavailable. */
  probe_input?: boolean | null
  tool: {
    /** Tool LinuxCNC considers loaded (0 for none). */
    in_spindle: number
    /** Tool selected by a T word and not yet changed to, or null. */
    prepared: number | null
  }
  program: ProgramStatus
  /** Changes whenever anything a preview depends on changes. */
  preview_context: number
  wait: Wait
  /** Active modal codes as LinuxCNC reports them, for example ["G1", "G54", "G90", "M3"]. */
  active_codes: string[]
  program_units: LengthUnit
}

export interface Tool {
  number: number
  pocket: number
  /** Z offset (tool length). */
  length: number
  diameter: number
  comment: string
}

export interface ToolTable {
  type: 'tool_table'
  revision: number
  tools: Tool[]
}

export interface ProgramText {
  type: 'program'
  revision: number
  file: string
  lines: string[]
  /** True when only a prefix is supplied. This is not the complete program or its line count. */
  truncated?: boolean
}

/** One contiguous run of motion from one source line, as the interpreter produced it. */
export interface PreviewSegment {
  line: number
  kind: 'rapid' | 'feed'
  tool: number
  /**
   * Polyline of tool tip positions in machine coordinates (the controlled point less the tool
   * offset in effect), flattened x, y, z; arcs arrive tessellated. The executable applies the tool
   * offset exactly once; the panel never applies offsets to preview points.
   */
  points: number[]
}

/**
 * The path the loaded program would take. Everything it depends on (program, work offsets, G92,
 * rotation, tool table, block delete and interpreter start state) has one revision,
 * `Status.preview_context`; a preview whose `context`
 * differs from it is out of date.
 */
export interface Preview {
  type: 'preview'
  /** The `Status.preview_context` this preview was computed for. */
  context: number
  /**
   * `pending`: being computed, `segments` empty. `failed` carries the interpreter's error;
   * `segments` then end where it stopped. `unavailable`: no program loaded, or no preview possible.
   */
  state: 'pending' | 'ready' | 'failed' | 'unavailable'
  error: { line: number; text: string } | null
  segments: PreviewSegment[]
  /** Extent of the feed moves (tool tip, machine coordinates), or null without any. */
  bounds: { min: Vec3; max: Vec3 } | null
}

/** All nine work coordinate systems, sent after `ready` and whenever values or availability change. */
export interface WorkOffsets {
  type: 'work_offsets'
  revision: number
  /** In order: G54 … G59.3. */
  systems: {
    wcs: number
    offset: Vec3
    rotation_xy: number
    /** False means the numeric fields are placeholders, not usable coordinates. Older peers
     * omit this field and supply a complete table. New senders always set it explicitly. */
    available?: boolean
  }[]
}

/** An operator error or message from LinuxCNC, or from the executable itself. */
export interface OperatorMessage {
  type: 'message'
  level: 'error' | 'info'
  text: string
  /** Milliseconds since the Unix epoch. */
  time: number
}

/** Final outcome of the command with this `id`. Every command gets exactly one. */
export interface CommandResult {
  type: 'result'
  id: number
  outcome:
    // LinuxCNC accepted the command; completion of any motion it starts is read from Status
    | 'accepted'
    | 'rejected' // not valid in the current state; nothing was sent to LinuxCNC
    | 'failed' // LinuxCNC reported an error
    | 'timed_out' // LinuxCNC did not answer
  /** Reason for anything but `accepted`, in words for the operator. */
  reason: string
}

export interface DirectoryEntry {
  name: string
  kind: 'directory' | 'file'
  bytes: number
  /** Milliseconds since the Unix epoch. */
  modified: number
}

export interface Directory {
  type: 'directory'
  path: string
  /** Null at the top of the program directory. */
  parent: string | null
  entries: DirectoryEntry[]
  /** True when the listing omits entries because a resource limit was reached. */
  truncated?: boolean
}

export type FromMachine =
  | Hello
  | Status
  | ToolTable
  | ProgramText
  | Preview
  | WorkOffsets
  | OperatorMessage
  | CommandResult
  | Directory

// ---- panel to machine -----------------------------------------------------------------------
// Every command carries an `id` the panel chooses; the matching `CommandResult` reports it.

interface CommandBase {
  id: number
}

/** The panel has loaded: send `hello`, the tool table, the program and status. */
export interface Ready extends CommandBase {
  type: 'ready'
}
/** `estop` asserts LinuxCNC's software E-stop. It is not the machine's E-stop circuit. */
export interface SetMachine extends CommandBase {
  type: 'set_machine'
  state: MachineState
}
export interface Home extends CommandBase {
  type: 'home'
  axis: AxisLetter | 'all'
}
/**
 * Continuous jog with a dead-man: the panel sends `held: true` every 50 ms while the control is
 * pressed and `held: false` on release. The executable stops the axis when no refresh arrives
 * for 150 ms.
 */
export interface Jog extends CommandBase {
  type: 'jog'
  axis: AxisLetter
  direction: -1 | 1
  /** mm/min, at most `Hello.max_jog_velocity`. */
  velocity: number
  held: boolean
}
export interface JogStep extends CommandBase {
  type: 'jog_step'
  axis: AxisLetter
  /** Signed distance, mm. */
  distance: number
  velocity: number
}
export interface JogStopAll extends CommandBase {
  type: 'jog_stop_all'
}
export interface Mdi extends CommandBase {
  type: 'mdi'
  text: string
}
export interface ListDirectory extends CommandBase {
  type: 'list_directory'
  /** Null for the program directory ([DISPLAY] PROGRAM_PREFIX). */
  path: string | null
}
export interface OpenProgram extends CommandBase {
  type: 'open_program'
  path: string
}
/** Runs the loaded program from its first line. */
export interface Run extends CommandBase {
  type: 'run'
}
export interface Pause extends CommandBase {
  type: 'pause'
}
export interface Resume extends CommandBase {
  type: 'resume'
}
/** Executes one block, then pauses. Starts the program when it is not running. */
export interface Step extends CommandBase {
  type: 'step'
}
/** Stops the program or MDI command and the spindle. Not an E-stop. */
export interface Abort extends CommandBase {
  type: 'abort'
}
export interface SetOverride extends CommandBase {
  type: 'set_override'
  which: 'feed' | 'rapid' | 'spindle'
  value: number
}
export interface SetSpindle extends CommandBase {
  type: 'set_spindle'
  direction: SpindleDirection
  /** Ignored for `stopped`. */
  rpm: number
}
export interface SetCoolant extends CommandBase {
  type: 'set_coolant'
  flood: boolean
  mist: boolean
}
export interface SetProgramOption extends CommandBase {
  type: 'set_program_option'
  option: 'optional_stop' | 'block_delete'
  on: boolean
}
/** Makes work coordinate system `wcs` (1 is G54 … 9 is G59.3) active. */
export interface SelectWcs extends CommandBase {
  type: 'select_wcs'
  wcs: number
}
/** Sets the work offset so the axis reads `value` at its present position (G10 L20). */
export interface TouchOff extends CommandBase {
  type: 'touch_off'
  axis: AxisLetter
  value: number
}
/**
 * Sets the loaded tool's length so Z reads `value` at its present position (G10 L10), and
 * applies it (G43).
 */
export interface ToolTouchOff extends CommandBase {
  type: 'tool_touch_off'
  value: number
}
/** Replaces one tool table row, or adds it. */
export interface SetTool extends CommandBase {
  type: 'set_tool'
  tool: Tool
}
export interface RemoveTool extends CommandBase {
  type: 'remove_tool'
  number: number
}
/** The operator has put the requested tool in the spindle (answers `Wait.tool_change`). */
export interface ToolChanged extends CommandBase {
  type: 'tool_changed'
}

export type ToMachine =
  | Ready
  | SetMachine
  | Home
  | Jog
  | JogStep
  | JogStopAll
  | Mdi
  | ListDirectory
  | OpenProgram
  | Run
  | Pause
  | Resume
  | Step
  | Abort
  | SetOverride
  | SetSpindle
  | SetCoolant
  | SetProgramOption
  | SelectWcs
  | TouchOff
  | ToolTouchOff
  | SetTool
  | RemoveTool
  | ToolChanged

/** A command without its `id`, as callers build it. */
export type CommandBody = ToMachine extends infer C ? (C extends ToMachine ? Omit<C, 'id'> : never) : never
