// A model of LinuxCNC's task behaviour for a three-axis mill, good enough to put the panel into
// every state it has to show. It speaks only protocol messages: `handle` takes a command, `emit`
// receives everything the machine says. Time advances only through `tick`, so tests are exact.

import type {
  AxisLetter,
  AxisStatus,
  CommandResult,
  FromMachine,
  Hello,
  InterpState,
  MachineState,
  Preview,
  RestartPlan,
  Status,
  TaskMode,
  ToMachine,
  Tool,
  Vec3,
  Wait,
} from '../protocol'
import { protocolVersion } from '../protocol'
import { axisJogVelocity, requiresHoming } from '../machine-capabilities'
import { defaultFiles, listDirectory, splitLines } from './files'
import {
  InterpError,
  activeCodes,
  axisIndices,
  cloneModal,
  interpretLine,
  newModal,
  previewAirCut,
  previewProgram,
  toWork,
  wcsNames,
  type AxisIndex,
  type Modal,
  type Op,
} from './gcode'

export const tickMs = 20
export const statusMs = 50
export const deadmanMs = 150
export const previewDelayMs = 150

/** Soft limits, which are also the physical travel of the simulated machine. */
export const travel: { readonly min: Vec3; readonly max: Vec3 } = { min: [0, 0, -200], max: [400, 250, 0] }
export const rapidVelocity = 6000
/** Feed for the last approach move of a restart, mm/min. */
export const restartPlungeFeed = 300
const homeVelocity = 4800
const minHomeSeconds = 1.5
const homePosition: Vec3 = [0, 0, 0]
const startPosition: Vec3 = [140, 90, -60]
/** Z first, so the tool is clear before X and Y move. */
const homeOrder: readonly AxisIndex[] = [2, 0, 1]

const letters: readonly AxisLetter[] = ['x', 'y', 'z']
const names: readonly string[] = ['X', 'Y', 'Z']

export const hello: Hello = {
  type: 'hello',
  protocol: protocolVersion,
  machine_name: 'Demo mill 400 × 250',
  simulated: true,
  quit_supported: true,
  axes: ['x', 'y', 'z'],
  machine_units: 'mm',
  jog_increments: [0.01, 0.1, 1, 10],
  max_jog_velocity: 3000,
  axis_max_jog_velocity: { x: 3000, y: 3000, z: 3000 },
  homing_required: true,
  max_feed_override: 1.5,
  min_spindle_override: 0.5,
  max_spindle_override: 1.5,
}

/**
 * What is wrong with the link, for testing the panel: `silent` is a frozen executable (nothing
 * is sent or answered), `stale` and `down` are link states the executable reports itself.
 */
export type LinkFault = 'none' | 'silent' | 'stale' | 'down'

type MoveOp = Extract<Op, { kind: 'move' }>

type Active =
  | { kind: 'move'; op: MoveOp; index: number }
  | { kind: 'dwell'; left: number }
  | { kind: 'tool_change'; tool: number }

interface Exec {
  source: 'program' | 'mdi'
  queue: Op[]
  active: Active | null
  /** Index of the next program line to interpret. */
  nextLine: number
  /** 1-based line being executed. */
  currentLine: number
  paused: boolean
  stepping: boolean
  /** A line with content has been interpreted and not yet completed. */
  ranLine: boolean
  /** Blocks a restart runs before `nextLine`, as MDI would. */
  preamble?: string[]
}

interface PlannedRestart {
  plan: RestartPlan
  context: number
  revision: number
  /** G0 to G3 in effect at the line, which the line may rely on. */
  motion: number
}

type JogState =
  | { kind: 'continuous'; velocity: number; refreshed: number }
  | { kind: 'step'; target: number; velocity: number }

interface Rejection {
  outcome: 'rejected' | 'failed'
  reason: string
}

function reject(reason: string): Rejection {
  return { outcome: 'rejected', reason }
}

function fail(reason: string): Rejection {
  return { outcome: 'failed', reason }
}

function defaultTools(): Tool[] {
  return [
    { number: 1, pocket: 1, length: 62.5, diameter: 50, comment: 'Face mill 50 mm' },
    { number: 2, pocket: 2, length: 48.2, diameter: 6, comment: 'End mill 6 mm, 3 flutes' },
    { number: 3, pocket: 3, length: 71, diameter: 5, comment: 'Drill 5 mm' },
    { number: 4, pocket: 4, length: 40.35, diameter: 8, comment: 'Spot drill 8 mm' },
  ]
}

export interface SimOptions {
  emit: (message: FromMachine) => void
  /** Wall clock for message time stamps. */
  clock?: () => number
  /** Replaces the built-in program directory. */
  files?: Map<string, string>
  /** Policy-only test configuration; this does not model another machine's mechanics. */
  capabilities?: Partial<Pick<Hello, 'max_jog_velocity' | 'axis_max_jog_velocity' | 'homing_required'>>
}

export class SimMachine {
  readonly modal: Modal = newModal(defaultTools())
  /** Suppresses status messages, for fast-forwarding. */
  quiet = false

  private readonly emitMessage: (message: FromMachine) => void
  private readonly clock: () => number
  private readonly files: Map<string, string>
  private readonly configuration: Hello

  private machine: MachineState = 'estop'
  private mode: TaskMode = 'manual'
  /** True position of the slides. */
  private phys: Vec3 = [...startPosition]
  /** Before homing, LinuxCNC counts from wherever the machine was switched on. */
  private readonly unhomedOffset: Vec3 = [...startPosition]
  private readonly homed: [boolean, boolean, boolean] = [false, false, false]
  private homeQueue: AxisIndex[] = []
  private homing: { axis: AxisIndex; from: number; elapsed: number; seconds: number } | null = null
  private readonly jogs: [JogState | null, JogState | null, JogState | null] = [null, null, null]
  private exec: Exec | null = null
  private velocity = 0
  private readonly overrides = { feed: 1, rapid: 1, spindle: 1 }
  private program: { file: string; lines: string[]; revision: number } | null = null
  /** Where the program last stopped before its end, for that program revision. */
  private stoppedAt: { revision: number; line: number; tool: number } | null = null
  private programRevision = 0
  private optionalStop = false
  private blockDelete = false
  private toolInSpindle = 0

  private now = 0
  private sinceStatus = 0
  private seq = 0
  private fault: LinkFault = 'none'
  private frozen: Status | null = null

  private toolsKey = ''
  private toolRevision = 0
  private offsetsKey = ''
  private offsetsRevision = 0
  private previewKey = ''
  private previewBlockDelete = false
  private previewPlanBroken = false
  private previewContext = 0
  private airLift = 0
  private airOffsetOwned = false
  private restarts = new Map<number, PlannedRestart>()
  private restartId = 0
  private plannedResult: RestartPlan | null = null
  private previewDue: number | null = null
  private preview: Preview = {
    type: 'preview',
    context: 0,
    state: 'unavailable',
    error: null,
    segments: [],
    bounds: null,
    dwells: [],
  }

  constructor(options: SimOptions) {
    this.configuration = structuredClone({ ...hello, ...options.capabilities })
    this.emitMessage = options.emit
    this.clock = options.clock ?? ((): number => Date.now())
    this.files = options.files ?? defaultFiles()
    // As if the last session's offsets came back from the var file.
    this.modal.offsets[0] = [150, 60, -160]
    this.modal.offsets[1] = [150, 160, -160]
    this.toolsKey = JSON.stringify(this.modal.tools)
    this.offsetsKey = JSON.stringify([this.modal.offsets, this.modal.rotations])
    this.syncModal()
    this.checkPreview()
  }

  // ---- time -----------------------------------------------------------------------------------

  tick(ms: number = tickMs): void {
    this.now += ms
    this.velocity = 0
    this.runHoming(ms)
    this.runJogs(ms)
    this.runExec(ms)
    this.checkTools()
    this.checkOffsets()
    this.checkPreview()
    this.sinceStatus += ms
    if (this.sinceStatus >= statusMs) {
      this.sinceStatus = 0
      this.sendStatus()
    }
  }

  /** Runs the machine for `ms` of its own time. */
  advance(ms: number): void {
    for (let t = 0; t < ms; t += tickMs) {
      this.tick()
    }
  }

  /** Runs until `done` holds; returns false when it did not within `limitMs`. */
  advanceUntil(done: (status: Status) => boolean, limitMs: number): boolean {
    for (let t = 0; t < limitMs; t += tickMs) {
      if (done(this.status())) {
        return true
      }
      this.tick()
    }
    return done(this.status())
  }

  setFault(fault: LinkFault): void {
    this.fault = fault
    this.frozen = fault === 'stale' ? this.status() : null
  }

  // ---- status ---------------------------------------------------------------------------------

  private reported(): Vec3 {
    const p: Vec3 = [...this.phys]
    for (const axis of axisIndices) {
      if (!this.homed[axis]) {
        p[axis] -= this.unhomedOffset[axis]
      }
    }
    return p
  }

  private wait(): Wait {
    const exec = this.exec
    if (exec === null) {
      return { kind: 'none' }
    }
    if (exec.active?.kind === 'tool_change') {
      return { kind: 'tool_change', tool: exec.active.tool }
    }
    if (exec.paused) {
      return { kind: 'paused' }
    }
    if (exec.active?.kind === 'dwell') {
      return { kind: 'dwell', seconds_left: Math.max(0, exec.active.left) }
    }
    return { kind: 'none' }
  }

  private interp(): InterpState {
    const exec = this.exec
    if (exec === null) {
      return 'idle'
    }
    if (exec.active?.kind === 'tool_change') {
      return 'waiting'
    }
    if (exec.paused) {
      return 'paused'
    }
    return exec.active?.kind === 'dwell' ? 'waiting' : 'reading'
  }

  status(): Status {
    const exec = this.exec
    const machine = this.reported()
    const work = toWork(this.modal, machine)
    const move = exec?.active?.kind === 'move' ? exec.active.op : null
    const end = move?.points.at(-1)
    const axes = axisIndices.map((axis): AxisStatus => {
      const jog = this.jogs[axis]
      let dtg = 0
      if (end !== undefined) {
        dtg = end[axis] - this.phys[axis]
      } else if (jog?.kind === 'step') {
        dtg = jog.target - this.phys[axis]
      }
      const homed = this.homed[axis]
      return {
        axis: letters[axis] ?? 'x',
        homed,
        homing: this.homing?.axis === axis,
        machine: machine[axis],
        work: work[axis],
        dtg,
        min_limit: travel.min[axis],
        max_limit: travel.max[axis],
        on_hard_limit: !homed && (this.phys[axis] <= travel.min[axis] || this.phys[axis] >= travel.max[axis]),
      }
    })
    const m = this.modal
    return {
      type: 'status',
      seq: this.seq,
      link: 'live',
      machine: this.machine,
      mode: this.mode,
      interp: this.interp(),
      axes,
      offsets: {
        wcs: m.wcs,
        g5x: [...(m.offsets[m.wcs - 1] ?? [0, 0, 0])],
        g92: [...m.g92],
        tool: [...m.toolOffset],
        rotation_xy: m.rotations[m.wcs - 1] ?? 0,
      },
      motion: {
        velocity: this.velocity,
        feed_mode: 'per_minute',
        programmed_feed: m.feed,
        feed_override: this.overrides.feed,
        rapid_override: this.overrides.rapid,
        in_position: this.inPosition(),
      },
      spindle: { direction: m.spindle, commanded_rpm: m.speed, override: this.overrides.spindle, actual_rpm: null, at_speed: null },
      probe_input: null,
      coolant: { flood: m.flood, mist: m.mist },
      tool: { in_spindle: this.toolInSpindle, prepared: m.prepared },
      program: {
        file: this.program?.file ?? null,
        revision: this.program?.revision ?? 0,
        current_line: exec?.source === 'program' ? exec.currentLine : 0,
        motion_line: exec?.source === 'program' && move !== null ? move.line : 0,
        optional_stop: this.optionalStop,
        block_delete: this.blockDelete,
        ...(this.airLift ? { air_z: this.airLift } : {}),
        ...(this.airOffsetOwned ? { air_offset_owned: true } : {}),
        ...(exec === null && this.stoppedAt !== null && this.stoppedAt.revision === this.program?.revision
          ? { restart_hint: { line: this.stoppedAt.line, tool: this.stoppedAt.tool } } : {}),
      },
      preview_context: this.previewContext,
      wait: this.wait(),
      active_codes: activeCodes(m),
      program_units: m.units,
    }
  }

  private sendStatus(): void {
    if (this.quiet || this.fault === 'silent') {
      return
    }
    this.seq += 1
    if (this.fault === 'stale' && this.frozen !== null) {
      this.emitMessage({ ...this.frozen, seq: this.seq, link: 'stale' })
    } else if (this.fault === 'down') {
      this.emitMessage({ ...blankStatus(), seq: this.seq, link: 'down' })
    } else {
      this.emitMessage(this.status())
    }
  }

  private emit(message: FromMachine): void {
    if (this.fault !== 'silent') {
      this.emitMessage(message)
    }
  }

  private operatorMessage(level: 'error' | 'info', text: string): void {
    this.emit({ type: 'message', level, text, time: this.clock() })
  }

  // ---- tool table and preview bookkeeping -------------------------------------------------------

  private checkTools(): void {
    const key = JSON.stringify(this.modal.tools)
    if (key !== this.toolsKey) {
      this.toolsKey = key
      this.toolRevision += 1
      this.sendTools()
    }
  }

  private sendTools(): void {
    this.emit({
      type: 'tool_table',
      revision: this.toolRevision,
      tools: this.modal.tools.map((t) => ({ ...t })),
    })
  }

  private checkOffsets(): void {
    const key = JSON.stringify([this.modal.offsets, this.modal.rotations])
    if (key !== this.offsetsKey) {
      this.offsetsKey = key
      this.offsetsRevision += 1
      this.sendOffsets()
    }
  }

  private sendOffsets(): void {
    this.emit({
      type: 'work_offsets',
      revision: this.offsetsRevision,
      systems: this.modal.offsets.map((offset, index) => ({
        wcs: index + 1,
        available: true,
        offset: [...offset],
        rotation_xy: this.modal.rotations[index] ?? 0,
      })),
    })
  }

  /** A new preview context whenever something the path depends on has changed. */
  private checkPreview(): void {
    const m = this.modal
    const programRunning = this.exec?.source === 'program'
    if (programRunning) {
      if (!this.previewPlanBroken && this.blockDelete !== this.previewBlockDelete) {
        this.previewPlanBroken = true
        this.preview = {
          type: 'preview',
          context: ++this.previewContext,
          state: 'unavailable',
          error: { line: 0, text: 'Block delete changed during the run. The preview updates when it ends.' },
          segments: [],
          bounds: null,
          dwells: [],
        }
        this.emit(this.preview)
      }
      return
    }
    if (this.previewPlanBroken) {
      this.previewPlanBroken = false
      this.previewKey = ''
    }
    this.previewBlockDelete = this.blockDelete
    const start = [
      this.reported(), m.wcs, m.units, m.absolute, m.motion,
      this.toolInSpindle, m.prepared, m.toolOffset, m.feed,
    ]
    const key = JSON.stringify([
      this.program?.revision ?? 0,
      this.blockDelete,
      m.offsets,
      m.rotations,
      m.g92,
      m.tools,
      this.airLift,
      start,
    ])
    if (key !== this.previewKey) {
      this.previewKey = key
      this.previewContext += 1
      this.previewDue = this.program === null ? null : this.now + previewDelayMs
      this.preview = {
        type: 'preview',
        context: this.previewContext,
        state: this.program === null ? 'unavailable' : 'pending',
        error: null,
        segments: [],
        bounds: null,
        dwells: [],
      }
      this.emit(this.preview)
    }
    if (this.previewDue !== null && this.now >= this.previewDue && this.program !== null) {
      this.previewDue = null
      // From where the machine is, not from where an MDI command in progress will end.
      const start = cloneModal(m)
      start.pos = this.reported()
      start.tool = this.toolInSpindle
      const checks = { travel, spindleLimit: this.configuration.max_spindle_rpm }
      const args = [start, this.program.lines, this.blockDelete, this.previewContext, checks] as const
      this.preview = this.airLift ? previewAirCut(...args, this.airLift) : previewProgram(...args)
      this.emit(this.preview)
    }
  }

  // ---- motion ---------------------------------------------------------------------------------

  private syncModal(): void {
    this.modal.pos = this.reported()
    this.modal.tool = this.toolInSpindle
  }

  /** No motion is in progress or interrupted: a move held by a pause is not in position. */
  private inPosition(): boolean {
    return (
      this.jogs.every((jog) => jog === null) &&
      this.homing === null &&
      this.homeQueue.length === 0 &&
      this.exec?.active?.kind !== 'move'
    )
  }

  private allHomed(): boolean {
    return this.homed.every((h) => h)
  }

  private runHoming(ms: number): void {
    if (this.homing === null) {
      const next = this.homeQueue.shift()
      if (next === undefined) {
        return
      }
      const from = this.phys[next]
      const seconds = Math.max(minHomeSeconds, (Math.abs(from - homePosition[next]) / homeVelocity) * 60)
      this.homed[next] = false
      this.homing = { axis: next, from, elapsed: 0, seconds }
    }
    const homing = this.homing
    homing.elapsed += ms / 1000
    const t = Math.min(1, homing.elapsed / homing.seconds)
    const target = homePosition[homing.axis]
    this.phys[homing.axis] = homing.from + (target - homing.from) * t
    this.velocity = (Math.abs(target - homing.from) / homing.seconds) * 60
    if (t >= 1) {
      this.homed[homing.axis] = true
      this.unhomedOffset[homing.axis] = 0
      this.homing = null
      this.velocity = 0
      this.syncModal()
    }
  }

  private runJogs(ms: number): void {
    for (const axis of axisIndices) {
      const jog = this.jogs[axis]
      if (jog === null) {
        continue
      }
      if (jog.kind === 'continuous' && this.now - jog.refreshed > deadmanMs) {
        this.jogs[axis] = null
        continue
      }
      const reach = (Math.abs(jog.velocity) / 60000) * ms
      const here = this.phys[axis]
      let target = jog.kind === 'continuous' ? here + Math.sign(jog.velocity) * reach : jog.target
      let done = false
      if (jog.kind === 'step') {
        if (Math.abs(target - here) > reach) {
          target = here + Math.sign(target - here) * reach
        } else {
          done = true
        }
      }
      const clamped = Math.min(travel.max[axis], Math.max(travel.min[axis], target))
      if (clamped !== target) {
        done = true
        if (!this.homed[axis]) {
          this.operatorMessage('error', `${names[axis] ?? ''} axis is on a hard limit; jog stopped`)
        }
      }
      this.phys[axis] = clamped
      this.velocity = Math.max(this.velocity, Math.abs(jog.velocity))
      if (done) {
        this.jogs[axis] = null
      }
    }
  }

  private stopJogs(): void {
    this.jogs.fill(null)
  }

  private limitError(op: MoveOp): string | null {
    for (const point of op.points) {
      for (const axis of axisIndices) {
        const side = point[axis] < travel.min[axis] - 1e-6 ? 'negative' : point[axis] > travel.max[axis] + 1e-6 ? 'positive' : null
        if (side !== null) {
          const where = op.line > 0 ? `on line ${String(op.line)}` : 'in MDI'
          return `${op.shape === 'arc' ? 'Arc' : 'Linear'} move ${where} would exceed ${names[axis] ?? ''}'s ${side} limit`
        }
      }
    }
    return null
  }

  /** Moves along the active polyline; returns the time left over, or -1 while it is not done. */
  private advanceMove(active: Extract<Active, { kind: 'move' }>, ms: number): number {
    const op = active.op
    const speed =
      op.motion === 'rapid'
        ? rapidVelocity * this.overrides.rapid
        : Math.min(rapidVelocity, op.feed * this.overrides.feed)
    if (speed <= 0) {
      return -1
    }
    const perMs = speed / 60000
    let reach = perMs * ms
    this.velocity = speed
    for (;;) {
      const target = op.points[active.index]
      if (target === undefined) {
        return reach / perMs
      }
      const distance = Math.hypot(target[0] - this.phys[0], target[1] - this.phys[1], target[2] - this.phys[2])
      if (distance > reach) {
        const f = reach / distance
        for (const axis of axisIndices) {
          this.phys[axis] += (target[axis] - this.phys[axis]) * f
        }
        return -1
      }
      this.phys = [...target]
      reach -= distance
      active.index += 1
    }
  }

  /** An interpreter or motion error: tell the operator and abort, as LinuxCNC does. */
  private execError(text: string): void {
    this.operatorMessage('error', text)
    this.abort()
  }

  private startOp(exec: Exec, op: Op): void {
    switch (op.kind) {
      case 'move': {
        const error = this.limitError(op)
        if (error === null) {
          exec.active = { kind: 'move', op, index: 1 }
        } else {
          this.execError(error)
        }
        break
      }
      case 'dwell':
        exec.active = { kind: 'dwell', left: op.seconds }
        break
      case 'tool_change':
        exec.active = { kind: 'tool_change', tool: op.tool }
        break
      case 'pause':
        if (!op.optional || this.optionalStop) {
          exec.paused = true
        }
        break
      case 'message':
        this.operatorMessage('info', op.text)
        break
      case 'end':
        this.exec = null
        this.stoppedAt = null
        this.clearAirOffset()
        break
    }
  }

  private runExec(ms: number): void {
    let left = ms
    for (let guard = 0; guard < 1000; guard += 1) {
      const exec = this.exec
      if (exec === null || exec.paused) {
        return
      }
      const active = exec.active
      if (active !== null) {
        if (active.kind === 'tool_change') {
          return
        }
        if (active.kind === 'dwell') {
          active.left -= left / 1000
          if (active.left > 0) {
            return
          }
          left = -active.left * 1000
        } else {
          left = this.advanceMove(active, left)
          if (left < 0) {
            return
          }
        }
        exec.active = null
        continue
      }
      const op = exec.queue.shift()
      if (op !== undefined) {
        this.startOp(exec, op)
        continue
      }
      if (exec.ranLine) {
        exec.ranLine = false
        if (exec.stepping) {
          exec.paused = true
          return
        }
      }
      if (exec.preamble !== undefined && exec.preamble.length > 0) {
        const block = exec.preamble.shift() ?? ''
        try {
          exec.queue = interpretLine(this.modal, block, exec.currentLine, false).ops.filter((op) => op.kind !== 'end')
        } catch (caught) {
          if (!(caught instanceof InterpError)) {
            throw caught
          }
          this.execError(caught.message)
          return
        }
        continue
      }
      if (exec.source === 'mdi' || this.program === null) {
        this.exec = null
        return
      }
      const text = this.program.lines[exec.nextLine]
      if (text === undefined) {
        this.execError('File ended with no percent sign or program end (M2 or M30)')
        return
      }
      exec.nextLine += 1
      exec.currentLine = exec.nextLine
      try {
        const result = interpretLine(this.modal, text, exec.currentLine, this.blockDelete)
        exec.queue = result.ops
        exec.ranLine = !result.blank
      } catch (caught) {
        if (!(caught instanceof InterpError)) {
          throw caught
        }
        this.execError(caught.message)
        return
      }
    }
  }

  /** Stops program, MDI, jogs, homing and the spindle. */
  private abort(): void {
    const exec = this.exec
    if (exec?.source === 'program' && this.program !== null && exec.currentLine > 0) {
      const line = exec.active?.kind === 'move' ? exec.active.op.line : exec.currentLine
      this.stoppedAt = { revision: this.program.revision, line, tool: this.toolInSpindle }
    }
    this.exec = null
    this.clearAirOffset()
    this.stopJogs()
    this.homeQueue = []
    this.homing = null
    this.modal.spindle = 'stopped'
    this.syncModal()
  }

  private clearAirOffset(): void {
    if (!this.airOffsetOwned) return
    this.modal.g92[2] = 0
    this.airLift = 0
    this.airOffsetOwned = false
  }

  // ---- commands -------------------------------------------------------------------------------

  handle(command: ToMachine): void {
    if (this.fault === 'silent') {
      return
    }
    this.plannedResult = null
    const rejection = this.execute(command)
    const restart = this.takePlannedResult()
    const result: CommandResult =
      rejection === null
        ? { type: 'result', id: command.id, outcome: 'accepted', reason: '', ...(restart === null ? {} : { restart }) }
        : { type: 'result', id: command.id, ...rejection }
    this.checkTools()
    this.checkOffsets()
    this.checkPreview()
    this.emit(result)
  }

  private notOn(what: string): Rejection | null {
    if (this.machine === 'estop') {
      return reject(`Can't ${what}: machine is in E-stop`)
    }
    if (this.machine === 'off') {
      return reject(`Can't ${what}: machine is off`)
    }
    return null
  }

  private busy(what: string): Rejection | null {
    if (this.exec !== null) {
      return reject(
        `Can't ${what} while ${this.exec.source === 'mdi' ? 'an MDI command' : 'a program'} is running`,
      )
    }
    if (this.homing !== null || this.homeQueue.length > 0) {
      return reject(`Can't ${what} while homing`)
    }
    return null
  }

  /** Runs G-code the executable issues itself (touch-off), always in millimetres. */
  private internal(text: string): Rejection | null {
    const units = this.modal.units
    this.syncModal()
    this.modal.units = 'mm'
    try {
      interpretLine(this.modal, text, 0, false)
    } catch (caught) {
      if (!(caught instanceof InterpError)) {
        throw caught
      }
      return fail(caught.detail)
    } finally {
      this.modal.units = units
    }
    return null
  }

  private execute(command: ToMachine): Rejection | null {
    switch (command.type) {
      case 'set_tool_model_defaults':
      case 'set_operator_settings':
      case 'set_tool_model_assignment':
        return reject('Native tool model storage is unavailable in the browser simulator')
      case 'ready':
        this.emit(this.configuration)
        this.sendTools()
        this.sendOffsets()
        if (this.program !== null) {
          this.emit({ type: 'program', ...this.program })
        }
        this.emit(this.preview)
        this.sendStatus()
        return null
      case 'quit':
        if (this.exec !== null) return reject('Stop the program before quitting.')
        // Exercise the shutdown request without closing the development browser or resetting E-stop.
        this.setMachine(this.machine === 'estop' ? 'estop' : 'off')
        this.sendStatus()
        this.emit({ type: 'message', level: 'info', text: 'Simulated quit: machine off; this browser stays open.', time: this.clock() })
        return null
      case 'set_machine':
        return this.setMachine(command.state)
      case 'home':
        return this.home(command.axis)
      case 'jog':
        return this.jog(command.axis, command.direction, command.velocity, command.held)
      case 'jog_step':
        return this.jogStep(command.axis, command.distance, command.velocity)
      case 'jog_stop_all':
        this.stopJogs()
        return null
      case 'mdi':
        return this.mdi(command.text)
      case 'list_directory': {
        const listing = listDirectory(this.files, command.path)
        if (listing === null) {
          return fail(`Can't list ${command.path ?? 'the program directory'}: no such directory`)
        }
        this.emit(listing)
        return null
      }
      case 'open_program':
        return this.open(command.path)
      case 'set_air_cut':
        if (!Number.isFinite(command.air_z) || command.air_z < 0 || command.air_z > 1000 ||
          command.air_z > 0 && command.air_z < 0.001) return reject('Air-cut lift is out of range.')
        if (this.exec !== null || !this.inPosition()) return reject('Stop the program before changing the air-cut lift.')
        if (command.air_z > 0 && this.program === null) return reject('Load a program first.')
        this.airLift = command.air_z
        return null
      case 'run': {
        if ((command.air_z ?? 0) !== this.airLift) return reject('The air-cut selection changed.')
        if (this.airLift && (command.preview_context === undefined || !command.confirmed || this.preview.state !== 'ready'))
          return reject(this.preview.check?.errors[0] ?? 'Review the raised preview before starting the air cut.')
        if (command.preview_context !== undefined) {
          if (this.preview.context !== command.preview_context || this.preview.state === 'pending')
            return reject('The program check changed. Review the current results.')
          const check = this.preview.check
          if (check?.errors[0]) return reject(check.errors[0])
          const warnings = !check || check.warnings.length > 0 ||
            check.units.some((unit) => command.display_units !== undefined && unit !== command.display_units)
          if (warnings && !command.confirmed) return reject('Review the program check warnings before starting.')
        }
        const rejection = this.run(false)
        if (!rejection && this.airLift) {
          this.modal.g92[2] = this.airLift
          this.airOffsetOwned = true
        }
        return rejection
      }
      case 'step':
        return this.airLift ? reject('Turn off air cut before using single block.') : this.step()
      case 'pause':
        if (this.exec === null) {
          return reject('Nothing is running')
        }
        if (this.exec.active?.kind === 'tool_change') {
          return reject('Waiting for the tool change')
        }
        if (this.exec.paused) {
          return reject('Already paused')
        }
        this.exec.paused = true
        return null
      case 'resume':
        if (this.exec?.paused !== true) {
          return reject('Program is not paused')
        }
        this.exec.paused = false
        this.exec.stepping = false
        return null
      case 'abort':
        this.abort()
        return null
      case 'set_override':
        return this.setOverride(command.which, command.value)
      case 'set_spindle': {
        const blocked = this.notOn('control the spindle') ?? this.busy('control the spindle')
        if (blocked !== null) {
          return blocked
        }
        if (command.direction !== 'stopped') {
          if (!(command.rpm > 0)) {
            return reject('Spindle speed must be above zero')
          }
          this.modal.speed = command.rpm
        }
        this.mode = 'manual'
        this.modal.spindle = command.direction
        return null
      }
      case 'set_coolant': {
        const blocked = this.notOn('switch coolant')
        if (blocked !== null) {
          return blocked
        }
        this.modal.flood = command.flood
        this.modal.mist = command.mist
        return null
      }
      case 'set_program_option':
        if (command.option === 'optional_stop') {
          this.optionalStop = command.on
        } else {
          if (this.airOffsetOwned) return reject('Stop the air cut before changing block delete.')
          this.blockDelete = command.on
        }
        return null
      case 'select_wcs': {
        if (!Number.isInteger(command.wcs) || command.wcs < 1 || command.wcs > 9) {
          return reject(`${String(command.wcs)} is not a work coordinate system (1 to 9)`)
        }
        const blocked = this.offsetCommand('select a coordinate system')
        if (blocked !== null) {
          return blocked
        }
        return this.internal(wcsNames[command.wcs - 1] ?? 'G54')
      }
      case 'touch_off': {
        if (!Number.isFinite(command.value)) {
          return reject('Touch-off value is not a number')
        }
        const blocked = this.offsetCommand('touch off')
        if (blocked !== null) {
          return blocked
        }
        return this.internal(`G10 L20 P0 ${command.axis.toUpperCase()}${command.value.toFixed(6)}`)
      }
      case 'set_work_offset': {
        if (!Number.isInteger(command.wcs) || command.wcs < 1 || command.wcs > 9 ||
            !command.offset.every(Number.isFinite)) return reject('Invalid work offset')
        const blocked = this.offsetCommand('set work offsets')
        if (blocked !== null) return blocked
        const decimal = (value: number): string => value.toLocaleString('en-US', { useGrouping: false, maximumFractionDigits: 20 })
        const [x, y, z] = command.offset
        return this.internal(`G10 L2 P${String(command.wcs)} X${decimal(x)} Y${decimal(y)} Z${decimal(z)}`)
      }
      case 'tool_touch_off': {
        if (!Number.isFinite(command.value)) {
          return reject('Touch-off value is not a number')
        }
        const blocked = this.offsetCommand('touch off the tool')
        if (blocked !== null) {
          return blocked
        }
        if (this.toolInSpindle === 0) {
          return reject("Can't touch off the tool: no tool in the spindle")
        }
        return (
          this.internal(`G10 L10 P${String(this.toolInSpindle)} Z${command.value.toFixed(6)}`) ??
          this.internal('G43')
        )
      }
      case 'fixed_setter':
        return reject('The fixed setter is unavailable in the browser simulator')
      case 'set_tool':
        return this.setTool(command.tool)
      case 'remove_tool': {
        const blocked = this.busy('edit the tool table')
        if (blocked !== null) {
          return blocked
        }
        if (command.number === this.toolInSpindle) {
          return reject(`Can't remove tool ${String(command.number)}: it is in the spindle`)
        }
        const index = this.modal.tools.findIndex((t) => t.number === command.number)
        if (index < 0) {
          return reject(`Tool ${String(command.number)} is not in the tool table`)
        }
        this.modal.tools.splice(index, 1)
        if (this.modal.prepared === command.number) {
          this.modal.prepared = null
        }
        return null
      }
      case 'plan_restart':
        return this.planRestart(command.line, command.preview_context)
      case 'start_restart':
        return this.startRestart(command.plan_id)
      case 'tool_changed': {
        const exec = this.exec
        if (exec?.active?.kind !== 'tool_change') {
          return reject('No tool change is pending')
        }
        this.toolInSpindle = exec.active.tool
        exec.active = null
        return null
      }
    }
  }

  /** Work offsets use MDI (G10), subject to the configured homing requirement. */
  private offsetCommand(what: string): Rejection | null {
    const blocked = this.notOn(what) ?? this.busy(what)
    if (blocked !== null) {
      return blocked
    }
    if (requiresHoming(this.configuration) && !this.allHomed()) {
      return reject(`Can't ${what} when not homed`)
    }
    this.mode = 'mdi'
    return null
  }

  private setMachine(state: MachineState): Rejection | null {
    if (state === 'on') {
      if (this.machine === 'estop') {
        return reject("Can't switch the machine on: reset the E-stop first")
      }
      this.machine = 'on'
      return null
    }
    this.machine = state
    this.abort()
    this.modal.flood = false
    this.modal.mist = false
    return null
  }

  private home(axis: AxisLetter | 'all'): Rejection | null {
    const blocked = this.notOn('home')
    if (blocked !== null) {
      return blocked
    }
    if (this.exec !== null) {
      return reject("Can't home while a program or MDI command is running")
    }
    this.stopJogs()
    this.mode = 'manual'
    const wanted = axis === 'all' ? [...homeOrder] : [letters.indexOf(axis) as AxisIndex]
    for (const index of wanted) {
      if (this.homing?.axis !== index && !this.homeQueue.includes(index)) {
        this.homeQueue.push(index)
      }
    }
    return null
  }

  private jogBlocked(): Rejection | null {
    const blocked = this.notOn('jog')
    if (blocked !== null) {
      return blocked
    }
    if (this.exec !== null) {
      return reject(`Can't jog while ${this.exec.source === 'mdi' ? 'an MDI command' : 'a program'} is running`)
    }
    if (this.homing !== null || this.homeQueue.length > 0) {
      return reject("Can't jog while an axis is homing")
    }
    return null
  }

  private jogVelocity(axis: AxisLetter, velocity: number): number {
    return axisJogVelocity(this.configuration, axis, velocity)
  }

  private jog(letter: AxisLetter, direction: -1 | 1, velocity: number, held: boolean): Rejection | null {
    const axis = letters.indexOf(letter) as AxisIndex
    if (!held) {
      if (this.jogs[axis]?.kind === 'continuous') {
        this.jogs[axis] = null
      }
      return null
    }
    const blocked = this.jogBlocked()
    if (blocked !== null) {
      return blocked
    }
    if (this.jogVelocity(letter, velocity) === 0) {
      return reject('Jog speed or configured axis limit is unavailable')
    }
    const limit = direction > 0 ? travel.max[axis] : travel.min[axis]
    if (this.phys[axis] === limit) {
      const side = direction > 0 ? 'positive' : 'negative'
      return reject(
        this.homed[axis]
          ? `${names[axis] ?? ''} is at its ${side} soft limit`
          : `${names[axis] ?? ''} is on its ${side} hard limit`,
      )
    }
    this.mode = 'manual'
    this.jogs[axis] = { kind: 'continuous', velocity: direction * this.jogVelocity(letter, velocity), refreshed: this.now }
    return null
  }

  private jogStep(letter: AxisLetter, distance: number, velocity: number): Rejection | null {
    const axis = letters.indexOf(letter) as AxisIndex
    const blocked = this.jogBlocked()
    if (blocked !== null) {
      return blocked
    }
    if (!this.inPosition()) {
      return reject('Wait for motion to stop before an incremental jog.')
    }
    if (this.jogVelocity(letter, velocity) === 0 || !Number.isFinite(distance)) {
      return reject('Jog step needs a distance and a speed above zero')
    }
    this.mode = 'manual'
    this.jogs[axis] = { kind: 'step', target: this.phys[axis] + distance, velocity: this.jogVelocity(letter, velocity) }
    return null
  }

  private mdi(text: string): Rejection | null {
    const blocked = this.notOn('issue MDI command')
    if (blocked !== null) {
      return blocked
    }
    if (requiresHoming(this.configuration) && !this.allHomed()) {
      return reject("Can't issue MDI command when not homed")
    }
    if (this.exec !== null) {
      return reject(
        `Can't issue MDI command while ${this.exec.source === 'mdi' ? 'another MDI command' : 'a program'} is running`,
      )
    }
    if (this.homing !== null || this.homeQueue.length > 0) {
      return reject("Can't issue MDI command while homing")
    }
    this.stopJogs()
    this.syncModal()
    let ops: Op[]
    try {
      ops = interpretLine(this.modal, text, 0, false).ops
    } catch (caught) {
      if (!(caught instanceof InterpError)) {
        throw caught
      }
      this.syncModal()
      return fail(caught.detail)
    }
    for (const op of ops) {
      const error = op.kind === 'move' ? this.limitError(op) : null
      if (error !== null) {
        this.syncModal()
        return fail(error)
      }
    }
    this.mode = 'mdi'
    this.exec = {
      source: 'mdi',
      queue: ops.filter((op) => op.kind !== 'end'),
      active: null,
      nextLine: 0,
      currentLine: 0,
      paused: false,
      stepping: false,
      ranLine: false,
    }
    return null
  }

  private open(path: string): Rejection | null {
    if (this.exec !== null) {
      return reject("Can't open a program while one is running")
    }
    const text = this.files.get(path)
    if (text === undefined) {
      return fail(`Can't open ${path}: no such file`)
    }
    this.programRevision += 1
    this.airLift = 0
    this.program = { file: path, lines: splitLines(text), revision: this.programRevision }
    this.emit({ type: 'program', ...this.program })
    return null
  }

  private run(stepping: boolean): Rejection | null {
    const what = stepping ? 'step' : 'run a program'
    const blocked = this.notOn(what)
    if (blocked !== null) {
      return blocked
    }
    if (requiresHoming(this.configuration) && !this.allHomed()) {
      return reject(`Can't ${what} when not homed`)
    }
    if (this.program === null) {
      return reject('No program is loaded')
    }
    if (this.exec !== null) {
      return reject(
        this.exec.source === 'mdi' ? "Can't run a program while an MDI command is running" : 'A program is already running',
      )
    }
    if (this.homing !== null || this.homeQueue.length > 0) {
      return reject(`Can't ${what} while homing`)
    }
    this.stopJogs()
    this.syncModal()
    this.stoppedAt = null
    this.mode = 'auto'
    this.exec = {
      source: 'program',
      queue: [],
      active: null,
      nextLine: 0,
      currentLine: 0,
      paused: false,
      stepping,
      ranLine: false,
    }
    return null
  }

  private takePlannedResult(): RestartPlan | null {
    const plan = this.plannedResult
    this.plannedResult = null
    return plan
  }

  /** Interprets the program up to `line` without motion and plans how to resume there. */
  private planRestart(line: number, context: number): Rejection | null {
    if (this.airLift) return reject('Turn off air cut before restarting from a line.')
    const blocked = this.notOn('restart a program') ?? this.busy('restart a program')
    if (blocked !== null) {
      return blocked
    }
    if (requiresHoming(this.configuration) && !this.allHomed()) {
      return reject("Can't restart a program when not homed")
    }
    const program = this.program
    if (program === null) {
      return reject('No program is loaded')
    }
    if (context !== this.previewContext || this.preview.context !== context || this.preview.state !== 'ready') {
      return reject('Wait for the preview of this program')
    }
    if (!Number.isInteger(line) || line < 1 || line > program.lines.length) {
      return reject(`Line ${String(line)} is not in the program`)
    }
    this.syncModal()
    const m = cloneModal(this.modal)
    m.pos = this.reported()
    m.tool = this.toolInSpindle
    let clearance = toWork(this.modal, this.reported())[2]
    try {
      for (let index = 0; index < line - 1; index += 1) {
        for (const op of interpretLine(m, program.lines[index] ?? '', index + 1, this.blockDelete).ops) {
          if (op.kind === 'end') {
            return reject(`The program ends before line ${String(line)}`)
          }
          if (op.kind === 'move') {
            for (const point of op.points) clearance = Math.max(clearance, toWork(m, point)[2])
          }
        }
      }
    } catch (caught) {
      if (!(caught instanceof InterpError)) {
        throw caught
      }
      return fail(caught.message)
    }
    const start = toWork(m, m.pos)
    // The approach comes down to the start, never up to it.
    clearance = Math.max(clearance, start[2])
    this.restartId += 1
    const plan: RestartPlan = {
      plan_id: this.restartId,
      line,
      tool: m.tool,
      needs_tool_change: m.tool !== this.toolInSpindle,
      wcs: m.wcs,
      h: m.toolOffset[2] !== 0 ? m.tool : 0,
      spindle_direction: m.spindle,
      spindle_rpm: m.spindle === 'stopped' ? 0 : m.speed,
      flood: m.flood,
      mist: m.mist,
      feed: m.feed,
      units: m.units,
      plane: 'XY',
      distance_mode: m.absolute ? 'absolute' : 'incremental',
      clearance_z: clearance,
      start,
      approach_feed: Math.min(m.feed > 0 ? m.feed : restartPlungeFeed, restartPlungeFeed),
    }
    this.restarts.clear()
    this.restarts.set(plan.plan_id, { plan, context, revision: program.revision, motion: m.motion })
    this.plannedResult = plan
    return null
  }

  /** Runs a plan: the tool change and approach as blocks, then the program from its line. */
  private startRestart(id: number): Rejection | null {
    const planned = this.restarts.get(id)
    if (planned === undefined) {
      return reject('This restart plan is no longer valid. Plan it again.')
    }
    if (planned.context !== this.previewContext || planned.revision !== this.program?.revision) {
      this.restarts.clear()
      return reject('The program or machine changed since the restart was planned. Plan it again.')
    }
    const started = this.run(false)
    if (started !== null || this.exec === null) {
      return started
    }
    this.restarts.clear()
    const { plan } = planned
    const number = (value: number): string => value.toFixed(4)
    const [x, y, z] = plan.start
    const spindle = plan.spindle_direction === 'forward' ? `S${number(plan.spindle_rpm)} M3`
      : plan.spindle_direction === 'reverse' ? `S${number(plan.spindle_rpm)} M4` : 'M5'
    const coolant = plan.flood || plan.mist ? [plan.mist ? 'M7' : '', plan.flood ? 'M8' : ''].join(' ').trim() : 'M9'
    // Lift first, change the tool, lift again for its length, then XY; the spindle and coolant
    // start just before the feed down to the start.
    this.exec.preamble = [
      `G21 G90 G17 ${wcsNames[plan.wcs - 1] ?? 'G54'}`,
      `G0 Z${number(plan.clearance_z)}`,
      ...(plan.needs_tool_change ? [`T${String(plan.tool)} M6`] : []),
      plan.h > 0 ? `G43 H${String(plan.h)}` : 'G49',
      `G0 Z${number(plan.clearance_z)}`,
      `G0 X${number(x)} Y${number(y)}`,
      spindle,
      coolant,
      `G1 Z${number(z)} F${number(plan.approach_feed)}`,
      [planned.motion <= 1 ? `G${String(planned.motion)}` : '', plan.units === 'inch' ? 'G20' : '', plan.distance_mode === 'incremental' ? 'G91' : '',
        plan.feed > 0 ? `F${number(plan.units === 'inch' ? plan.feed / 25.4 : plan.feed)}` : ''].join(' ').trim(),
    ].filter((block) => block !== '')
    this.exec.nextLine = plan.line - 1
    this.exec.currentLine = plan.line
    return null
  }

  private step(): Rejection | null {
    const exec = this.exec
    if (exec === null) {
      return this.run(true)
    }
    if (exec.source === 'mdi') {
      return reject("Can't step an MDI command")
    }
    if (exec.active?.kind === 'tool_change') {
      return reject('Waiting for the tool change')
    }
    exec.stepping = true
    exec.paused = false
    return null
  }

  private setOverride(which: 'feed' | 'rapid' | 'spindle', value: number): Rejection | null {
    if (!Number.isFinite(value)) {
      return reject('Override value is not a number')
    }
    const clamp = (low: number, high: number): number => Math.min(high, Math.max(low, value))
    if (which === 'feed') {
      this.overrides.feed = clamp(0, hello.max_feed_override)
    } else if (which === 'rapid') {
      this.overrides.rapid = clamp(0, 1)
    } else {
      this.overrides.spindle = clamp(hello.min_spindle_override, hello.max_spindle_override)
    }
    return null
  }

  private setTool(tool: Tool): Rejection | null {
    const blocked = this.busy('edit the tool table')
    if (blocked !== null) {
      return blocked
    }
    if (!Number.isInteger(tool.number) || tool.number < 1) {
      return reject('Tool number must be a whole number from 1')
    }
    if (!Number.isFinite(tool.length) || !Number.isFinite(tool.diameter) || tool.diameter < 0) {
      return reject('Tool length and diameter must be numbers, the diameter not negative')
    }
    const tools = this.modal.tools
    const index = tools.findIndex((t) => t.number === tool.number)
    if (index < 0) {
      tools.push({ ...tool })
      tools.sort((a, b) => a.number - b.number)
    } else {
      tools[index] = { ...tool }
    }
    return null
  }
}

function blankStatus(): Status {
  return {
    type: 'status',
    seq: 0,
    link: 'down',
    machine: 'estop',
    mode: 'manual',
    interp: 'idle',
    axes: letters.map((axis) => ({
      axis,
      homed: false,
      homing: false,
      machine: 0,
      work: 0,
      dtg: 0,
      min_limit: 0,
      max_limit: 0,
      on_hard_limit: false,
    })),
    offsets: { wcs: 1, g5x: [0, 0, 0], g92: [0, 0, 0], tool: [0, 0, 0], rotation_xy: 0 },
    motion: {
      velocity: 0,
      feed_mode: 'per_minute',
      programmed_feed: 0,
      feed_override: 1,
      rapid_override: 1,
      in_position: true,
    },
    spindle: { direction: 'stopped', commanded_rpm: 0, override: 1, actual_rpm: null, at_speed: null },
    probe_input: null,
    coolant: { flood: false, mist: false },
    tool: { in_spindle: 0, prepared: null },
    program: { file: null, revision: 0, current_line: 0, motion_line: 0, optional_stop: false, block_delete: false },
    preview_context: 0,
    wait: { kind: 'none' },
    active_codes: [],
    program_units: 'mm',
  }
}
