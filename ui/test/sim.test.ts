import { afterEach, describe, expect, it, vi } from 'vitest'

import type { FromMachine, Status } from '../src/protocol'
import { createSimulator } from '../src/sim'
import { programDirectory, samplesDirectory } from '../src/sim/files'
import { SimMachine, deadmanMs, hello, rapidVelocity, travel } from '../src/sim/machine'
import { applyScene, sceneNames } from '../src/sim/scenes'
import { machinePosition, rig, work } from './harness'

describe('machine state', () => {
  it('starts in E-stop with nothing homed, reading zero', () => {
    const { status } = rig()
    expect(status().machine).toBe('estop')
    expect(status().axes.map((axis) => axis.homed)).toEqual([false, false, false])
    expect(machinePosition(status())).toEqual([0, 0, 0])
    expect(status().interp).toBe('idle')
  })

  it('goes estop → off → on, and not straight to on', () => {
    const { send, status } = rig()
    expect(send({ type: 'set_machine', state: 'on' })).toMatchObject({
      outcome: 'rejected',
      reason: "Can't switch the machine on: reset the E-stop first",
    })
    expect(send({ type: 'set_machine', state: 'off' }).outcome).toBe('accepted')
    expect(status().machine).toBe('off')
    expect(send({ type: 'set_machine', state: 'on' }).outcome).toBe('accepted')
    expect(status().machine).toBe('on')
  })

  it('E-stop and machine off abort everything and stop spindle and coolant', () => {
    for (const state of ['estop', 'off'] as const) {
      const r = rig()
      r.home()
      r.send({ type: 'set_spindle', direction: 'forward', rpm: 1000 })
      r.send({ type: 'set_coolant', flood: true, mist: true })
      r.load('G1 X100 F100\nM2')
      r.send({ type: 'run' })
      r.sim.advance(500)
      expect(r.status().interp).toBe('reading')
      r.send({ type: 'set_machine', state })
      const status = r.status()
      expect(status.machine).toBe(state)
      expect(status.interp).toBe('idle')
      expect(status.spindle.direction).toBe('stopped')
      expect(status.coolant).toEqual({ flood: false, mist: false })
      const before = machinePosition(r.status())
      r.sim.advance(500)
      expect(machinePosition(r.status())).toEqual(before)
    }
  })
})

describe('homing', () => {
  it('takes seconds per axis and sets the machine position', () => {
    const r = rig()
    r.send({ type: 'set_machine', state: 'off' })
    r.send({ type: 'set_machine', state: 'on' })
    expect(r.send({ type: 'home', axis: 'z' }).outcome).toBe('accepted')
    r.sim.advance(1000)
    expect(r.status().axes[2]).toMatchObject({ homed: false, homing: true })
    r.sim.advance(1000)
    expect(r.status().axes[2]).toMatchObject({ homed: true, homing: false, machine: 0 })
    expect(r.status().axes[0]?.homed).toBe(false)
    r.send({ type: 'home', axis: 'all' })
    r.sim.advance(6000)
    expect(r.status().axes.map((axis) => axis.homed)).toEqual([true, true, true])
    expect(machinePosition(r.status())).toEqual([0, 0, 0])
  })

  it('needs the machine on', () => {
    const r = rig()
    expect(r.send({ type: 'home', axis: 'all' })).toMatchObject({
      outcome: 'rejected',
      reason: "Can't home: machine is in E-stop",
    })
  })
})

describe('rejections', () => {
  it('jog needs the machine on and an idle interpreter', () => {
    const r = rig()
    const jog = { type: 'jog', axis: 'x', direction: 1, velocity: 600, held: true } as const
    expect(r.send(jog).reason).toBe("Can't jog: machine is in E-stop")
    r.send({ type: 'set_machine', state: 'off' })
    expect(r.send(jog).reason).toBe("Can't jog: machine is off")
    expect(r.send({ type: 'jog_step', axis: 'x', distance: 1, velocity: 600 }).reason).toBe("Can't jog: machine is off")
    r.home()
    r.load('G1 X100 F100\nM2')
    r.send({ type: 'run' })
    expect(r.send(jog)).toMatchObject({ outcome: 'rejected', reason: "Can't jog while a program is running" })
    r.send({ type: 'abort' })
    r.send({ type: 'mdi', text: 'G1 X50 F100' })
    expect(r.send(jog).reason).toBe("Can't jog while an MDI command is running")
  })

  it('MDI needs the machine on, all axes homed and an idle interpreter', () => {
    const r = rig()
    expect(r.send({ type: 'mdi', text: 'G0 X1' }).reason).toBe("Can't issue MDI command: machine is in E-stop")
    r.send({ type: 'set_machine', state: 'off' })
    r.send({ type: 'set_machine', state: 'on' })
    expect(r.send({ type: 'mdi', text: 'G0 X1' })).toMatchObject({
      outcome: 'rejected',
      reason: "Can't issue MDI command when not homed",
    })
    r.home()
    r.load('G1 X100 F100\nM2')
    r.send({ type: 'run' })
    expect(r.send({ type: 'mdi', text: 'G0 X1' }).reason).toBe("Can't issue MDI command while a program is running")
  })

  it('run needs the machine on, all axes homed and a loaded program', () => {
    const r = rig()
    expect(r.send({ type: 'run' }).reason).toBe("Can't run a program: machine is in E-stop")
    r.send({ type: 'set_machine', state: 'off' })
    r.send({ type: 'set_machine', state: 'on' })
    expect(r.send({ type: 'run' }).reason).toBe("Can't run a program when not homed")
    expect(r.send({ type: 'step' }).reason).toBe("Can't step when not homed")
    r.home()
    expect(r.send({ type: 'run' })).toMatchObject({ outcome: 'rejected', reason: 'No program is loaded' })
    r.load('G1 X100 F100\nM2')
    expect(r.send({ type: 'run' }).outcome).toBe('accepted')
    expect(r.status().mode).toBe('auto')
    expect(r.send({ type: 'run' }).reason).toBe('A program is already running')
  })

  it('resume needs a pause, pause needs something running, tool_changed needs a request', () => {
    const r = rig()
    r.home()
    expect(r.send({ type: 'resume' })).toMatchObject({ outcome: 'rejected', reason: 'Program is not paused' })
    expect(r.send({ type: 'pause' }).reason).toBe('Nothing is running')
    expect(r.send({ type: 'tool_changed' }).reason).toBe('No tool change is pending')
  })

  it('offsets, tool table and program cannot change under a running program', () => {
    const r = rig()
    r.send({ type: 'set_machine', state: 'off' })
    r.send({ type: 'set_machine', state: 'on' })
    expect(r.send({ type: 'touch_off', axis: 'x', value: 0 }).reason).toBe("Can't touch off when not homed")
    expect(r.send({ type: 'select_wcs', wcs: 2 }).reason).toBe("Can't select a coordinate system when not homed")
    r.home()
    expect(r.send({ type: 'tool_touch_off', value: 0 }).reason).toBe("Can't touch off the tool: no tool in the spindle")
    r.load('G1 X100 F100\nM2')
    r.send({ type: 'run' })
    expect(r.send({ type: 'touch_off', axis: 'x', value: 0 }).reason).toBe("Can't touch off while a program is running")
    expect(r.send({ type: 'open_program', path: `${programDirectory}/slot.ngc` }).reason).toBe(
      "Can't open a program while one is running",
    )
    expect(r.send({ type: 'remove_tool', number: 4 }).reason).toBe("Can't edit the tool table while a program is running")
    expect(r.send({ type: 'set_spindle', direction: 'forward', rpm: 100 }).reason).toBe(
      "Can't control the spindle while a program is running",
    )
    expect(r.send({ type: 'home', axis: 'x' }).reason).toBe("Can't home while a program or MDI command is running")
  })

  it('files and directories that do not exist fail', () => {
    const r = rig()
    expect(r.send({ type: 'open_program', path: '/nowhere.ngc' })).toMatchObject({
      outcome: 'failed',
      reason: "Can't open /nowhere.ngc: no such file",
    })
    expect(r.send({ type: 'list_directory', path: '/etc' }).outcome).toBe('failed')
  })
})

describe('jogging', () => {
  it('moves while refreshed and stops by itself 150 ms after the last refresh', () => {
    const r = rig()
    r.send({ type: 'set_machine', state: 'off' })
    r.send({ type: 'set_machine', state: 'on' })
    const jog = { type: 'jog', axis: 'x', direction: 1, velocity: 600, held: true } as const
    for (let n = 0; n < 10; n += 1) {
      expect(r.send(jog).outcome).toBe('accepted')
      r.sim.advance(40)
    }
    // 400 ms at 10 mm/s.
    expect(r.status().axes[0]?.machine).toBeCloseTo(4, 6)
    expect(r.status().mode).toBe('manual')
    r.sim.advance(1000)
    // The last refresh was 40 ms before the 400 ms mark; the axis ran on to the dead-man time,
    // give or take a tick.
    expect(r.status().axes[0]?.machine).toBeCloseTo(((400 - 40 + deadmanMs) / 1000) * 10, 0)
    expect(r.status().axes[0]?.machine).toBeLessThanOrEqual(((400 - 40 + deadmanMs) / 1000) * 10)
    const stopped = r.status().axes[0]?.machine
    r.sim.advance(1000)
    expect(r.status().axes[0]?.machine).toBe(stopped)
  })

  it('stops at once on release and on stop all', () => {
    const r = rig()
    r.home()
    r.send({ type: 'jog', axis: 'y', direction: 1, velocity: 600, held: true })
    r.sim.advance(100)
    r.send({ type: 'jog', axis: 'y', direction: 1, velocity: 600, held: false })
    const y = r.status().axes[1]?.machine
    r.sim.advance(100)
    expect(r.status().axes[1]?.machine).toBe(y)
    r.send({ type: 'jog_step', axis: 'x', distance: 50, velocity: 600 })
    r.send({ type: 'jog', axis: 'y', direction: 1, velocity: 600, held: true })
    r.sim.advance(60)
    r.send({ type: 'jog_stop_all' })
    const before = machinePosition(r.status())
    r.sim.advance(500)
    expect(machinePosition(r.status())).toEqual(before)
  })

  it('clamps the speed to the fastest jog the machine offers', () => {
    const r = rig()
    r.home()
    r.send({ type: 'jog', axis: 'x', direction: 1, velocity: 1e9, held: true })
    r.sim.advance(100)
    expect(r.status().axes[0]?.machine).toBeCloseTo((hello.max_jog_velocity / 60) * 0.1, 6)
  })

  it('steps by the distance asked for and reports the distance to go', () => {
    const r = rig()
    r.home()
    expect(r.send({ type: 'jog_step', axis: 'x', distance: 1, velocity: 600 }).outcome).toBe('accepted')
    r.sim.advance(40)
    expect(r.status().axes[0]?.dtg).toBeCloseTo(0.6, 6)
    r.sim.advance(1000)
    expect(machinePosition(r.status())).toEqual([1, 0, 0])
    r.send({ type: 'jog_step', axis: 'z', distance: -0.01, velocity: 600 })
    r.sim.advance(1000)
    expect(machinePosition(r.status())).toEqual([1, 0, -0.01])
  })

  it('refuses an incremental jog while the machine is not in position', () => {
    const r = rig()
    r.home()
    const step = { type: 'jog_step', axis: 'x', distance: 1, velocity: 600 } as const
    expect(r.send(step).outcome).toBe('accepted')
    r.sim.advance(40)
    expect(r.status().motion.in_position).toBe(false)
    expect(r.send(step)).toMatchObject({
      outcome: 'rejected',
      reason: 'Wait for motion to stop before an incremental jog.',
    })
    expect(r.send({ ...step, axis: 'y' }).reason).toBe('Wait for motion to stop before an incremental jog.')
    r.sim.advance(1000)
    // The refused steps were not queued: one step was made.
    expect(machinePosition(r.status())).toEqual([1, 0, 0])
    expect(r.status().motion.in_position).toBe(true)
    r.send({ type: 'jog', axis: 'y', direction: 1, velocity: 600, held: true })
    expect(r.send(step).reason).toBe('Wait for motion to stop before an incremental jog.')
    r.send({ type: 'jog', axis: 'y', direction: 1, velocity: 600, held: false })
    expect(r.send(step).outcome).toBe('accepted')
  })

  it('refuses every jog while any axis is homing', () => {
    const r = rig()
    r.home()
    r.send({ type: 'home', axis: 'z' })
    r.sim.advance(100)
    expect(r.status().axes[2]?.homing).toBe(true)
    for (const axis of ['x', 'y', 'z'] as const) {
      expect(r.send({ type: 'jog', axis, direction: 1, velocity: 600, held: true })).toMatchObject({
        outcome: 'rejected',
        reason: "Can't jog while an axis is homing",
      })
      expect(r.send({ type: 'jog_step', axis, distance: 1, velocity: 600 }).reason).toBe(
        "Can't jog while an axis is homing",
      )
    }
    r.sim.advance(3000)
    expect(r.send({ type: 'jog', axis: 'x', direction: 1, velocity: 600, held: true }).outcome).toBe('accepted')
  })

  it('soft limits stop a jog and refuse one that starts on the limit', () => {
    const r = rig()
    r.home()
    expect(r.send({ type: 'jog', axis: 'x', direction: -1, velocity: 600, held: true })).toMatchObject({
      outcome: 'rejected',
      reason: 'X is at its negative soft limit',
    })
    // A homed step past the travel is refused, as the native command policy does; one that
    // ends exactly on the limit runs.
    expect(r.send({ type: 'jog_step', axis: 'y', distance: 1000, velocity: 3000 })).toMatchObject({
      outcome: 'rejected', reason: 'Incremental jog exceeds configured travel.' })
    expect(r.send({ type: 'jog_step', axis: 'y', distance: 0, velocity: 3000 }).outcome).toBe('rejected')
    expect(r.status().axes[1]?.machine).toBe(0)
    r.send({ type: 'jog_step', axis: 'y', distance: travel.max[1], velocity: 3000 })
    r.sim.advance(20_000)
    expect(r.status().axes[1]?.machine).toBe(travel.max[1])
    expect(r.send({ type: 'jog', axis: 'y', direction: 1, velocity: 600, held: true }).reason).toBe(
      'Y is at its positive soft limit',
    )
  })
})

describe('the dead-man through the timer-driven transport', () => {
  afterEach(() => {
    vi.useRealTimers()
  })

  it('stops a continuous jog when the panel stops refreshing', () => {
    vi.useFakeTimers()
    const transport = createSimulator(new URLSearchParams())
    const got: Status[] = []
    transport.listen((message: FromMachine) => {
      if (message.type === 'status') {
        got.push(message)
      }
    })
    const x = (): number => got.at(-1)?.axes[0]?.machine ?? NaN
    transport.send({ type: 'ready', id: 1 })
    transport.send({ type: 'set_machine', state: 'off', id: 2 })
    transport.send({ type: 'set_machine', state: 'on', id: 3 })
    let id = 4
    const refresh = window.setInterval(() => {
      id += 1
      transport.send({ type: 'jog', axis: 'x', direction: 1, velocity: 1200, held: true, id })
    }, 50)
    vi.advanceTimersByTime(1000)
    expect(x()).toBeGreaterThan(15)
    window.clearInterval(refresh)
    // The panel died here: no release is ever sent.
    vi.advanceTimersByTime(deadmanMs + 100)
    const stopped = x()
    vi.advanceTimersByTime(2000)
    expect(x()).toBe(stopped)
    expect(stopped).toBeLessThan(15 + 1200 / 60)
  })
})

describe('MDI', () => {
  it('runs a command and returns to idle', () => {
    const r = rig()
    r.home()
    expect(r.send({ type: 'mdi', text: 'G0 X10 Y5 Z-2' }).outcome).toBe('accepted')
    expect(r.status().mode).toBe('mdi')
    expect(r.status().interp).toBe('reading')
    r.finish()
    expect(work(r.status())).toEqual([10, 5, -2])
  })

  it('fails with the interpreter error and leaves the machine where it was', () => {
    const r = rig()
    r.home()
    expect(r.send({ type: 'mdi', text: 'G41 D999' })).toMatchObject({
      outcome: 'failed',
      reason: 'G41: tool 999 needs a positive diameter in the tool table',
    })
    expect(r.send({ type: 'mdi', text: 'G53 G0 Z50' })).toMatchObject({
      outcome: 'failed',
      reason: "Linear move in MDI would exceed Z's positive limit",
    })
    expect(r.status().interp).toBe('idle')
    expect(r.send({ type: 'mdi', text: 'G0 X1' }).outcome).toBe('accepted')
    r.finish()
    expect(machinePosition(r.status())).toEqual([151, 0, 0])
  })
})

describe('program execution', () => {
  it('moves at the programmed feed times the feed override, and reports lines and distance to go', () => {
    const r = rig()
    r.home()
    r.send({ type: 'mdi', text: 'G0 X0 Y0 Z10' })
    r.finish()
    r.load('G1 X60 F600\nG1 Y30\nM2')
    r.send({ type: 'run' })
    r.sim.advance(1000)
    let status = r.status()
    expect(status.axes[0]?.work).toBeCloseTo(10, 0)
    expect(status.axes[0]?.dtg).toBeCloseTo(50, 0)
    expect(status.motion.velocity).toBe(600)
    expect(status.motion.programmed_feed).toBe(600)
    expect(status.motion.feed_mode).toBe('per_minute')
    expect(status.program).toMatchObject({ current_line: 1, motion_line: 1 })
    r.send({ type: 'set_override', which: 'feed', value: 0.5 })
    r.sim.advance(1000)
    status = r.status()
    expect(status.axes[0]?.work).toBeCloseTo(15, 0)
    expect(status.motion.velocity).toBe(300)
    r.send({ type: 'set_override', which: 'feed', value: 0 })
    r.sim.advance(1000)
    expect(r.status().axes[0]?.work).toBeCloseTo(15, 0)
    expect(r.status().motion.velocity).toBe(0)
    r.send({ type: 'set_override', which: 'feed', value: 1 })
    r.finish()
    expect(work(r.status())).toEqual([60, 30, 10])
    expect(r.status().program).toMatchObject({ current_line: 0, motion_line: 0 })
    expect(r.errors()).toEqual([])
  })

  it('rapids run at the machine maximum times the rapid override', () => {
    const r = rig()
    r.home()
    r.load('G53 G0 X300\nM2')
    r.send({ type: 'set_override', which: 'rapid', value: 0.25 })
    r.send({ type: 'run' })
    r.sim.advance(1000)
    expect(r.status().motion.velocity).toBe(rapidVelocity * 0.25)
    expect(r.status().axes[0]?.machine).toBeCloseTo((rapidVelocity * 0.25) / 60, 0)
  })

  it('caps both rapid and feed independently of their overrides', () => {
    const r = rig()
    r.home()
    r.load('G53 G0 X20\nG53 G1 X40 F1800\nM2')
    expect(r.send({ type: 'set_override', which: 'velocity_limit', value: 0.1 }).outcome).toBe('accepted')
    expect(r.send({ type: 'set_override', which: 'velocity_limit', value: 1.1 }).outcome).toBe('rejected')
    r.send({ type: 'run' })
    r.sim.advance(1000)
    expect(r.status().motion).toMatchObject({ velocity: 600, velocity_limit: 0.1, rapid_override: 1, feed_override: 1 })
    r.sim.advance(1500)
    expect(r.status().program.motion_line).toBe(2)
    expect(r.status().motion.velocity).toBe(600)
    r.send({ type: 'set_override', which: 'velocity_limit', value: 1 })
    r.sim.advance(100)
    expect(r.status().motion.velocity).toBe(1800)
  })

  it('pauses, resumes and aborts', () => {
    const r = rig()
    r.home()
    r.load('G1 X100 F600\nM2')
    r.send({ type: 'set_spindle', direction: 'forward', rpm: 1000 })
    r.send({ type: 'run' })
    r.sim.advance(500)
    expect(r.send({ type: 'pause' }).outcome).toBe('accepted')
    expect(r.status()).toMatchObject({ interp: 'paused', wait: { kind: 'paused' } })
    expect(r.status().spindle.direction).toBe('forward')
    const held = machinePosition(r.status())
    r.sim.advance(500)
    expect(machinePosition(r.status())).toEqual(held)
    expect(r.status().program.motion_line).toBe(1)
    expect(r.send({ type: 'pause' }).reason).toBe('Already paused')
    expect(r.send({ type: 'resume' }).outcome).toBe('accepted')
    r.sim.advance(500)
    expect(r.status().interp).toBe('reading')
    expect(r.status().axes[0]?.machine).toBeGreaterThan(held[0] ?? NaN)
    expect(r.send({ type: 'abort' }).outcome).toBe('accepted')
    expect(r.status()).toMatchObject({ interp: 'idle', wait: { kind: 'none' } })
    expect(r.status().spindle.direction).toBe('stopped')
    const stopped = machinePosition(r.status())
    r.sim.advance(500)
    expect(machinePosition(r.status())).toEqual(stopped)
  })

  it('a pause in the middle of a move is not in position, and step finishes that block', () => {
    const r = rig()
    r.home()
    r.load('G53 G1 X60 F600\nG53 G1 X120\nM2')
    r.send({ type: 'run' })
    r.sim.advance(1000)
    r.send({ type: 'pause' })
    r.sim.advance(200)
    let status = r.status()
    expect(status.motion).toMatchObject({ velocity: 0, in_position: false })
    expect(status.axes[0]?.dtg).toBeCloseTo(50, 0)
    expect(status.program.motion_line).toBe(1)
    expect(r.send({ type: 'step' }).outcome).toBe('accepted')
    r.sim.advance(300)
    expect(r.status().interp).toBe('reading')
    r.sim.advance(10_000)
    status = r.status()
    // The interrupted block was finished, the next one not started.
    expect(machinePosition(status)).toEqual([60, 0, 0])
    expect(status).toMatchObject({ interp: 'paused', wait: { kind: 'paused' } })
    expect(status.motion).toMatchObject({ velocity: 0, in_position: true })
    expect(status.program.current_line).toBe(1)
    r.send({ type: 'step' })
    r.sim.advance(10_000)
    expect(machinePosition(r.status())).toEqual([120, 0, 0])
    expect(r.status().interp).toBe('paused')
  })

  it('single step executes one block and pauses, from idle and from a pause', () => {
    const r = rig()
    r.home()
    r.load('(header)\n\nG53 G0 X10\nG53 G0 X20\nG53 G0 X30\nM2')
    expect(r.send({ type: 'step' }).outcome).toBe('accepted')
    r.sim.advance(3000)
    expect(r.status()).toMatchObject({ interp: 'paused', wait: { kind: 'paused' } })
    expect(machinePosition(r.status())).toEqual([10, 0, 0])
    expect(r.status().program.current_line).toBe(3)
    r.send({ type: 'step' })
    r.sim.advance(3000)
    expect(machinePosition(r.status())).toEqual([20, 0, 0])
    expect(r.status().interp).toBe('paused')
    r.send({ type: 'resume' })
    r.finish()
    expect(machinePosition(r.status())).toEqual([30, 0, 0])
  })

  it('waits for the tool change and loads the tool on confirmation', () => {
    const r = rig()
    r.home()
    r.load('S500 M3\nT2 M6\nG43\nG0 X10\nM2')
    r.send({ type: 'run' })
    r.sim.advance(200)
    let status = r.status()
    expect(status.wait).toEqual({ kind: 'tool_change', tool: 2 })
    expect(status.interp).toBe('waiting')
    expect(status.tool).toEqual({ in_spindle: 0, prepared: null })
    expect(status.spindle.direction).toBe('stopped')
    expect(r.send({ type: 'pause' }).reason).toBe('Waiting for the tool change')
    r.sim.advance(5000)
    expect(r.status().wait.kind).toBe('tool_change')
    expect(r.send({ type: 'tool_changed' }).outcome).toBe('accepted')
    r.sim.advance(40)
    status = r.status()
    expect(status.tool.in_spindle).toBe(2)
    expect(status.offsets.tool).toEqual([0, 0, 48.2])
    r.finish()
    expect(r.status().active_codes).toContain('G43')
  })

  it('reports a prepared tool until it is changed to', () => {
    const r = rig()
    r.home()
    r.send({ type: 'mdi', text: 'T3' })
    r.finish()
    expect(r.status().tool).toEqual({ in_spindle: 0, prepared: 3 })
  })

  it('aborting a tool change leaves the old tool loaded', () => {
    const r = rig()
    r.home()
    r.load('T2 M6\nM2')
    r.send({ type: 'run' })
    r.sim.advance(100)
    r.send({ type: 'abort' })
    expect(r.status().tool.in_spindle).toBe(0)
    expect(r.status().wait.kind).toBe('none')
  })

  it('M1 pauses only with optional stop on; M0 always pauses', () => {
    const program = 'G53 G0 X10\nM1\nG53 G0 X20\nM0\nG53 G0 X30\nM2'
    const off = rig()
    off.home()
    off.load(program)
    off.send({ type: 'run' })
    off.sim.advance(5000)
    expect(off.status()).toMatchObject({ wait: { kind: 'paused' }, program: { current_line: 4 } })
    expect(machinePosition(off.status())).toEqual([20, 0, 0])

    const on = rig()
    on.home()
    on.load(program)
    expect(on.send({ type: 'set_program_option', option: 'optional_stop', on: true }).outcome).toBe('accepted')
    expect(on.status().program.optional_stop).toBe(true)
    on.send({ type: 'run' })
    on.sim.advance(5000)
    expect(on.status()).toMatchObject({ wait: { kind: 'paused' }, program: { current_line: 2 } })
    on.send({ type: 'resume' })
    on.sim.advance(5000)
    expect(on.status().program.current_line).toBe(4)
    on.send({ type: 'resume' })
    on.finish()
    expect(machinePosition(on.status())).toEqual([30, 0, 0])
  })

  it('block delete skips deleted blocks and changes the preview', () => {
    const r = rig()
    r.home()
    r.load('G53 G0 X10\n/ G53 G0 X20\nM2')
    expect(r.preview().segments).toHaveLength(2)
    r.send({ type: 'set_program_option', option: 'block_delete', on: true })
    r.sim.advance(400)
    expect(r.preview().segments).toHaveLength(1)
    r.send({ type: 'run' })
    r.finish()
    expect(machinePosition(r.status())).toEqual([10, 0, 0])
  })

  it('G4 dwells and counts the seconds down', () => {
    const r = rig()
    r.home()
    r.load('G4 P2\nM2')
    r.send({ type: 'run' })
    r.sim.advance(500)
    const wait = r.status().wait
    expect(wait.kind).toBe('dwell')
    expect(wait.kind === 'dwell' ? wait.seconds_left : NaN).toBeCloseTo(1.5, 1)
    expect(r.status().interp).toBe('waiting')
    r.sim.advance(1600)
    expect(r.status().interp).toBe('idle')
  })

  it('a move beyond a soft limit stops the program with an operator error', () => {
    const r = rig()
    r.home()
    r.load('G0 X10\nG1 Z-100 F500\nG0 X20\nM2')
    expect(r.preview().state).toBe('ready')
    r.send({ type: 'set_spindle', direction: 'forward', rpm: 500 })
    r.send({ type: 'run' })
    r.finish()
    expect(r.errors()).toEqual(["Linear move on line 2 would exceed Z's negative limit"])
    expect(r.status().interp).toBe('idle')
    expect(r.status().spindle.direction).toBe('stopped')
    expect(work(r.status())[0]).toBe(10)
    expect(r.status().axes[2]?.machine).toBe(0)
  })

  it('an interpreter error stops the running program with an operator error', () => {
    const r = rig()
    r.home()
    r.load('G53 G0 X10\nG41 D999\nG53 G0 X20\nM2')
    expect(r.preview()).toMatchObject({
      state: 'failed',
      error: { line: 2, text: 'line 2: G41: tool 999 needs a positive diameter in the tool table' },
    })
    expect(r.send({ type: 'run' }).outcome).toBe('accepted')
    r.finish()
    expect(r.errors()).toEqual(['line 2: G41: tool 999 needs a positive diameter in the tool table'])
    expect(machinePosition(r.status())).toEqual([10, 0, 0])
  })

  it('a program without an end stops with LinuxCNC’s error', () => {
    const r = rig()
    r.home()
    r.load('G53 G0 X10')
    r.send({ type: 'run' })
    r.finish()
    expect(r.errors()).toEqual(['File ended with no percent sign or program end (M2 or M30)'])
  })

  it('shows (MSG, …) comments to the operator', () => {
    const r = rig()
    r.home()
    r.load('(MSG, hello)\nM2')
    r.send({ type: 'run' })
    r.finish()
    expect(r.messages().map((message) => [message.level, message.text])).toEqual([['info', 'hello']])
  })
})

describe('the built-in programs', () => {
  it.each(['plate-face-pocket.ngc', 'drill-pattern-inch.ngc', 'slot.ngc'])('%s previews and runs to its end', (name) => {
    const r = rig()
    r.home()
    const opened = r.send({ type: 'open_program', path: `${programDirectory}/${name}` })
    expect(opened.outcome).toBe('accepted')
    r.sim.advance(400)
    expect(r.preview()).toMatchObject({ state: 'ready', error: null })
    expect(r.preview().segments.length).toBeGreaterThan(10)
    r.send({ type: 'run' })
    r.finish()
    expect(r.errors()).toEqual([])
    expect(r.status().spindle.direction).toBe('stopped')
  })

  it('the LinuxCNC samples load, fail their preview and stop when run', () => {
    const r = rig()
    r.home()
    const path = `${samplesDirectory}/gmoccapy_2_tools_with_cutter_radius_compensation.ngc`
    expect(r.send({ type: 'open_program', path }).outcome).toBe('accepted')
    r.sim.advance(400)
    const program = r.out.findLast((message) => message.type === 'program')
    expect(program).toMatchObject({ file: path })
    expect(program?.type === 'program' ? program.lines.length : 0).toBe(93)
    expect(r.preview()).toMatchObject({
      state: 'failed',
      error: { line: 10, text: 'line 10: G61 is not supported by the simulator' },
    })
    r.send({ type: 'run' })
    r.finish()
    expect(r.errors()).toEqual(['line 10: G61 is not supported by the simulator'])
  })

  it('lists the program directory and the samples below it', () => {
    const r = rig()
    r.send({ type: 'list_directory', path: null })
    const top = r.out.findLast((message) => message.type === 'directory')
    expect(top).toMatchObject({ path: programDirectory, parent: null })
    expect(top?.type === 'directory' ? top.entries.map((entry) => [entry.name, entry.kind]) : []).toEqual([
      ['linuxcnc-samples', 'directory'],
      ['drill-pattern-inch.ngc', 'file'],
      ['plate-face-pocket.ngc', 'file'],
      ['slot.ngc', 'file'],
    ])
    r.send({ type: 'list_directory', path: samplesDirectory })
    const samples = r.out.findLast((message) => message.type === 'directory')
    expect(samples).toMatchObject({ parent: programDirectory })
    expect(samples?.type === 'directory' ? samples.entries : []).toHaveLength(3)
  })
})

describe('offsets and tools', () => {
  it('marks inactive systems not current after a program until the mode leaves auto, as the native adapter', () => {
    const r = rig(); r.home()
    const systems = (): { wcs: number; current?: boolean; offset: number[] }[] => {
      const table = r.out.findLast((message) => message.type === 'work_offsets')
      return table?.type === 'work_offsets' ? table.systems : []
    }
    r.send({ type: 'set_work_offset', wcs: 3, offset: [1, 2, 3] })
    expect(systems().map((system) => system.current)).toEqual(Array(9).fill(true))
    r.load('G10 L2 P3 X5\nG1 X10 F600\nM2')
    r.send({ type: 'run' })
    r.sim.advance(200)
    expect(systems().map((system) => system.current)).toEqual([true, false, false, false, false, false, false, false, false])
    r.finish()
    expect(r.status().mode).toBe('auto')
    expect(systems()[2]).toMatchObject({ current: false, offset: [5, 2, 3] })
    r.send({ type: 'mdi', text: 'G54' }); r.finish()
    expect(systems().map((system) => system.current)).toEqual(Array(9).fill(true))
  })
  it('sets and zeroes the XY rotation and clears G92/G52 as the native commands, refused while busy', () => {
    const r = rig(); r.home()
    r.send({ type: 'mdi', text: 'G53 G0 X100 Y50' }); r.finish()
    r.send({ type: 'touch_off', axis: 'x', value: 0 }); r.send({ type: 'touch_off', axis: 'y', value: 0 })
    expect(r.send({ type: 'set_work_rotation', wcs: 1, rotation_xy: 90 }).outcome).toBe('accepted')
    expect(r.status().offsets.rotation_xy).toBe(90)
    expect(r.status().offsets.g5x.slice(0, 2)).toEqual([100, 50])
    r.send({ type: 'mdi', text: 'G53 G0 X100 Y60' }); r.finish()
    // Rotated 90°, the work X axis points along machine +Y.
    expect(work(r.status())[0]).toBeCloseTo(10, 6)
    expect(work(r.status())[1]).toBeCloseTo(0, 6)
    expect(r.send({ type: 'set_work_rotation', wcs: 3, rotation_xy: -12.5 }).outcome).toBe('accepted')
    const table = r.out.findLast((message) => message.type === 'work_offsets')
    expect(table?.type === 'work_offsets' ? table.systems[2]?.rotation_xy : null).toBe(-12.5)
    expect(r.send({ type: 'set_work_rotation', wcs: 1, rotation_xy: 0 }).outcome).toBe('accepted')
    expect(r.status().offsets.rotation_xy).toBe(0)
    expect(r.send({ type: 'set_work_rotation', wcs: 1, rotation_xy: 400 }).reason).toBe('Invalid XY rotation')
    r.send({ type: 'mdi', text: 'G92 X5 Y5' }); r.finish()
    expect(r.status().offsets.g92).not.toEqual([0, 0, 0])
    expect(r.send({ type: 'clear_g92' }).outcome).toBe('accepted')
    expect(r.status().offsets.g92).toEqual([0, 0, 0])
    const history = r.out.findLast((message) => message.type === 'offset_history')
    expect(history?.type === 'offset_history' ? history.entries[0] : null).toMatchObject({ source: 'work_offset_table', target: 'g92' })
    r.load('G1 X10 F100\nM2')
    r.send({ type: 'run' })
    r.sim.advance(200)
    expect(r.send({ type: 'clear_g92' }).reason).toBe("Can't clear the G92/G52 shift while a program is running")
    expect(r.send({ type: 'set_work_rotation', wcs: 1, rotation_xy: 5 }).reason).toBe("Can't set an XY rotation while a program is running")
  })

  it('edits, copies and clears explicit XYZ origins without moving or changing modal context', () => {
    const r = rig(); r.home()
    r.send({ type: 'mdi', text: 'G10 L2 P2 R15' }); r.finish()
    r.send({ type: 'mdi', text: 'G92 X7 Y8 Z9' }); r.finish()
    r.send({ type: 'mdi', text: 'G20 G91' }); r.finish()
    const before = r.status()
    for (const offset of [[25.4, -50.8, 3.5], [1, 2, 3], [0, 0, 0]] as const) {
      expect(r.send({ type: 'set_work_offset', wcs: 2, offset: [...offset] }).outcome).toBe('accepted')
      const table = r.out.findLast((message) => message.type === 'work_offsets')
      expect(table?.type === 'work_offsets' ? table.systems[1] : null).toMatchObject({ wcs: 2, offset: [...offset], rotation_xy: 15 })
      expect(machinePosition(r.status())).toEqual(machinePosition(before))
      expect(r.status().offsets.wcs).toBe(before.offsets.wcs)
      expect(r.status().offsets.g92).toEqual(before.offsets.g92)
      expect(r.status().program_units).toBe('inch')
      expect(r.status().active_codes).toContain('G91')
    }
    r.send({ type: 'set_machine', state: 'off' })
    expect(r.send({ type: 'set_work_offset', wcs: 2, offset: [1, 2, 3] }).outcome).toBe('rejected')
  })
  it('touch-off makes the axis read the value in the active system', () => {
    const r = rig()
    r.home()
    r.send({ type: 'mdi', text: 'G53 G0 X200 Y100 Z-50' })
    r.finish()
    expect(r.send({ type: 'touch_off', axis: 'x', value: 0 }).outcome).toBe('accepted')
    expect(r.send({ type: 'touch_off', axis: 'z', value: 12.5 }).outcome).toBe('accepted')
    const status = r.status()
    expect(work(status)).toEqual([0, 40, 12.5])
    expect(status.offsets.g5x).toEqual([200, 60, -62.5])
    const offsets = r.out.findLast((message) => message.type === 'work_offsets')
    expect(offsets?.type === 'work_offsets' ? offsets.systems[0] : null).toEqual({
      wcs: 1,
      available: true,
      current: true,
      offset: [200, 60, -62.5],
      rotation_xy: 0,
    })
  })

  it('touch-off is in millimetres even after an inch program', () => {
    const r = rig()
    r.home()
    r.send({ type: 'mdi', text: 'G20' })
    r.finish()
    r.send({ type: 'touch_off', axis: 'x', value: 10 })
    expect(work(r.status())[0]).toBe(10)
    expect(r.status().program_units).toBe('inch')
  })

  it('selecting a system changes the reading, not the position', () => {
    const r = rig()
    r.home()
    const before = machinePosition(r.status())
    expect(r.send({ type: 'select_wcs', wcs: 2 }).outcome).toBe('accepted')
    expect(r.status().offsets.wcs).toBe(2)
    expect(r.status().active_codes).toContain('G55')
    expect(work(r.status())).toEqual([-150, -160, 160])
    expect(machinePosition(r.status())).toEqual(before)
    expect(r.send({ type: 'select_wcs', wcs: 10 }).outcome).toBe('rejected')
  })

  it('tool touch-off sets the loaded tool’s length and applies it', () => {
    const r = rig()
    r.home()
    r.send({ type: 'mdi', text: 'T2 M6' })
    r.sim.advance(40)
    r.send({ type: 'tool_changed' })
    r.finish()
    r.send({ type: 'mdi', text: 'G53 G0 Z-100' })
    r.finish()
    expect(r.send({ type: 'tool_touch_off', value: 0 }).outcome).toBe('accepted')
    expect(r.status().offsets.tool).toEqual([0, 0, 60])
    expect(work(r.status())[2]).toBe(0)
    r.sim.tick()
    const table = r.out.findLast((message) => message.type === 'tool_table')
    expect(table?.type === 'tool_table' ? table.tools[1]?.length : null).toBe(60)
  })

  it('edits, adds and removes tool table rows', () => {
    const r = rig()
    r.home()
    const tool = { number: 7, pocket: 7, length: 33.3, diameter: 3, comment: 'engraver' }
    expect(r.send({ type: 'set_tool', tool }).outcome).toBe('accepted')
    expect(r.send({ type: 'set_tool', tool: { ...tool, number: 2, comment: 'replaced' } }).outcome).toBe('accepted')
    let table = r.out.findLast((message) => message.type === 'tool_table')
    expect(table?.type === 'tool_table' ? table.tools.map((t) => [t.number, t.comment]) : []).toEqual([
      [1, 'Face mill 50 mm'],
      [2, 'replaced'],
      [3, 'Drill 5 mm'],
      [4, 'Spot drill 8 mm'],
      [7, 'engraver'],
    ])
    expect(r.send({ type: 'remove_tool', number: 7 }).outcome).toBe('accepted')
    expect(r.send({ type: 'remove_tool', number: 7 }).reason).toBe('Tool 7 is not in the tool table')
    expect(r.send({ type: 'set_tool', tool: { ...tool, number: 0 } }).outcome).toBe('rejected')
    table = r.out.findLast((message) => message.type === 'tool_table')
    expect(table?.type === 'tool_table' ? table.tools : []).toHaveLength(4)
    r.send({ type: 'mdi', text: 'T1 M6' })
    r.sim.advance(40)
    r.send({ type: 'tool_changed' })
    r.finish()
    expect(r.send({ type: 'remove_tool', number: 1 }).reason).toBe("Can't remove tool 1: it is in the spindle")
  })
})

describe('preview context', () => {
  it('reports no preview without a program', () => {
    const r = rig()
    r.send({ type: 'ready' })
    expect(r.preview()).toMatchObject({ state: 'unavailable', context: r.status().preview_context })
    expect(r.out.some((message) => message.type === 'hello')).toBe(true)
    expect(r.out.some((message) => message.type === 'tool_table')).toBe(true)
    expect(r.out.some((message) => message.type === 'work_offsets')).toBe(true)
  })

  it('goes pending, then ready, for each new context', () => {
    const r = rig()
    r.home()
    const before = r.status().preview_context
    r.send({ type: 'open_program', path: `${programDirectory}/slot.ngc` })
    const context = r.status().preview_context
    expect(context).toBe(before + 1)
    expect(r.preview()).toMatchObject({ state: 'pending', context, segments: [] })
    r.sim.advance(100)
    expect(r.preview().state).toBe('pending')
    r.sim.advance(100)
    expect(r.preview()).toMatchObject({ state: 'ready', context })
  })

  it('changes with work offsets and the tool table, and the path moves with them', () => {
    const r = rig()
    r.home()
    r.load('G0 X0 Y0 Z0\nG1 X10 F100\nM2')
    const first = r.preview()
    expect(first.segments[1]?.points).toEqual([150, 60, -160, 160, 60, -160])
    r.send({ type: 'touch_off', axis: 'x', value: 100 })
    expect(r.status().preview_context).toBe(first.context + 1)
    expect(r.preview().state).toBe('pending')
    r.sim.advance(200)
    expect(r.preview().segments[1]?.points).toEqual([-100, 60, -160, -90, 60, -160])
    const second = r.status().preview_context
    r.send({ type: 'set_tool', tool: { number: 9, pocket: 9, length: 1, diameter: 1, comment: '' } })
    expect(r.status().preview_context).toBe(second + 1)
  })

  it('does not change while a program moves the machine', () => {
    const r = rig()
    r.home()
    r.load('T1 M6\nG43\nG10 L2 P2 X160\nG55\nG0 X1\nG4 P1\nM2')
    const context = r.status().preview_context
    r.send({ type: 'run' })
    r.sim.advance(100)
    r.send({ type: 'tool_changed' })
    r.sim.advance(500)
    expect(r.status().offsets.wcs).toBe(2)
    expect(r.status().offsets.g5x[0]).toBe(160)
    expect(r.preview()).toMatchObject({ state: 'ready', context })
    expect(r.status().preview_context).toBe(context)
  })

  it('recomputes the initial move after jogging changes the program start position', () => {
    const r = rig()
    r.home()
    r.load('G1 X10 F100\nM2')
    const before = r.preview()
    expect(r.send({ type: 'jog_step', axis: 'y', distance: 5, velocity: 600 }).outcome).toBe('accepted')
    r.sim.advance(1000)
    expect(r.preview().context).toBeGreaterThan(before.context)
    expect(r.preview().state).toBe('ready')
    expect(r.preview().segments[0]?.points).toEqual([0, 5, 0, 160, 5, 0])
  })

  it('recomputes inherited motion mode even when MDI did not move the machine', () => {
    const r = rig()
    r.home()
    r.load('X10 F100\nM2')
    expect(r.preview().segments[0]?.kind).toBe('feed')
    const context = r.preview().context
    expect(r.send({ type: 'mdi', text: 'G0' }).outcome).toBe('accepted')
    r.sim.advance(400)
    expect(r.preview().context).toBeGreaterThan(context)
    expect(r.preview().segments[0]?.kind).toBe('rapid')
  })

  it('recomputes a program that inherits its prepared tool from MDI', () => {
    const r = rig()
    r.home()
    r.load('M6\nG0 X10\nM2')
    expect(r.preview().state).toBe('failed')
    expect(r.send({ type: 'mdi', text: 'T2' }).outcome).toBe('accepted')
    r.sim.advance(400)
    expect(r.status().tool).toEqual({ in_spindle: 0, prepared: 2 })
    expect(r.preview().state).toBe('ready')
    expect(r.preview().segments[0]?.tool).toBe(2)
  })
})

describe('overrides', () => {
  it('clamp to the limits the machine announced', () => {
    const r = rig()
    const set = (which: 'feed' | 'rapid' | 'spindle', value: number): Status => {
      expect(r.send({ type: 'set_override', which, value }).outcome).toBe('accepted')
      return r.status()
    }
    expect(set('feed', 9).motion.feed_override).toBe(hello.max_feed_override)
    expect(set('feed', -1).motion.feed_override).toBe(0)
    expect(set('rapid', 2).motion.rapid_override).toBe(1)
    expect(set('spindle', 0).spindle.override).toBe(hello.min_spindle_override)
    expect(set('spindle', 9).spindle.override).toBe(hello.max_spindle_override)
    expect(set('spindle', 1.2).spindle.override).toBe(1.2)
  })
})

describe('link faults', () => {
  function statuses(fault: 'silent' | 'stale' | 'down'): Status[] {
    const out: FromMachine[] = []
    const sim = new SimMachine({
      emit: (message): void => {
        out.push(message)
      },
    })
    sim.handle({ type: 'set_machine', state: 'off', id: 1 })
    sim.advance(200)
    sim.setFault(fault)
    out.length = 0
    sim.handle({ type: 'set_machine', state: 'on', id: 2 })
    sim.advance(200)
    if (fault === 'silent') {
      expect(out).toEqual([])
    } else {
      expect(out.some((message) => message.type === 'result')).toBe(true)
    }
    return out.filter((message) => message.type === 'status')
  }

  it('silent: nothing is sent or answered', () => {
    expect(statuses('silent')).toEqual([])
  })

  it('stale: the last status task wrote keeps arriving, marked stale', () => {
    const got = statuses('stale')
    expect(got.length).toBeGreaterThanOrEqual(3)
    expect(got.every((status) => status.link === 'stale' && status.machine === 'off')).toBe(true)
    expect(got[1]?.seq).toBe((got[0]?.seq ?? 0) + 1)
  })

  it('down: placeholders marked down', () => {
    const got = statuses('down')
    expect(got.every((status) => status.link === 'down' && status.machine === 'estop')).toBe(true)
  })
})

describe('scenes', () => {
  function scene(name: string): { status: Status; out: FromMachine[] } {
    const out: FromMachine[] = []
    const sim = new SimMachine({
      emit: (message): void => {
        out.push(message)
      },
    })
    applyScene(sim, name)
    return { status: sim.status(), out }
  }

  it.each(sceneNames)('%s sends no status while fast-forwarding', (name) => {
    expect(scene(name).out.some((message) => message.type === 'status')).toBe(false)
  })

  it('reach the states they are named after', () => {
    expect(scene('estop').status.machine).toBe('estop')
    expect(scene('off').status.machine).toBe('off')
    expect(scene('unhomed').status).toMatchObject({ machine: 'on' })
    expect(scene('homing').status.axes.some((axis) => axis.homing)).toBe(true)
    expect(work(scene('manual').status)).toEqual([-12.4, 8, 22.4])
    expect(scene('loaded').status.program.file).toBe(`${programDirectory}/plate-face-pocket.ngc`)
    expect(scene('running').status).toMatchObject({ interp: 'reading', tool: { in_spindle: 2 } })
    expect(scene('paused').status).toMatchObject({ interp: 'paused', wait: { kind: 'paused' } })
    expect(scene('toolchange').status).toMatchObject({ wait: { kind: 'tool_change', tool: 2 }, tool: { in_spindle: 1 } })
    expect(scene('optstop').status).toMatchObject({ wait: { kind: 'paused' }, program: { current_line: 23 } })
    expect(scene('dwell').status.wait.kind).toBe('dwell')
    const failed = scene('failed-preview')
    expect(failed.out.findLast((message) => message.type === 'preview')).toMatchObject({ state: 'failed' })
    const error = scene('error')
    expect(error.status.interp).toBe('idle')
    expect(error.out.filter((message) => message.type === 'message')).toMatchObject([
      { level: 'error', text: 'line 10: G61 is not supported by the simulator' },
    ])
    expect(scene('limit').out.filter((message) => message.type === 'message')).toMatchObject([
      { level: 'error', text: "Linear move on line 12 would exceed X's negative limit" },
    ])
  })
})
