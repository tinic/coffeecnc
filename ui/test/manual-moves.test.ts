import { describe, expect, it } from 'vitest'
import { shallowRef } from 'vue'

import { arrived, goToPlan, limitMarginMm, moveBlock, nextPositionName, powerFeedPlan, safeZBlock, saveBlock, type MoveContext, type PowerFeedRequest } from '../src/manual-moves'
import { createMoveSequence } from '../src/move-sequence'
import type { CommandBody, CommandResult, Status } from '../src/protocol'
import type { ResultListener } from '../src/store'
import { SimMachine } from '../src/sim/machine'

/** On, homed and idle at X 100 Y 50 Z -20 machine; limits X 0..400, Y 0..250, Z -200..0. */
function status(change: Partial<Status> = {}): Status {
  const base = new SimMachine({ emit: (): void => undefined, clock: (): number => 0 }).status()
  const at = { x: 100, y: 50, z: -20 }
  return { ...base, machine: 'on', interp: 'idle', program_units: 'mm',
    axes: base.axes.map((axis) => ({ ...axis, homed: true, machine: at[axis.axis] })), ...change }
}
const live = (s: Status = status()): MoveContext => ({ status: s, machineBlock: null })
const feed = (change: Partial<PowerFeedRequest> = {}): PowerFeedRequest =>
  ({ axis: 'x', direction: 1, extent: { kind: 'limit' }, feed: 300, maxFeed: 6000, ...change })
const moved = (s: Status, at: Partial<Record<'x' | 'y' | 'z', number>>, interp: Status['interp'] = 'idle'): Status =>
  ({ ...s, interp, axes: s.axes.map((axis) => ({ ...axis, machine: at[axis.axis] ?? axis.machine })) })

describe('power feed target', () => {
  it('feeds to just inside the soft limit in either direction', () => {
    const plus = powerFeedPlan(live(), feed())
    expect(plus).toEqual({ ok: true, plan: { axis: 'x', direction: 1, start: 100, target: 400 - limitMarginMm, feed: 300,
      text: 'G21 G90 G94 G53 G1 X399.500 F300' } })
    const minus = powerFeedPlan(live(), feed({ axis: 'z', direction: -1 }))
    expect(minus.ok && minus.plan.text).toBe('G21 G90 G94 G53 G1 Z-199.500 F300')
  })
  it('feeds a set distance and refuses one that would pass the limit margin', () => {
    const distance = powerFeedPlan(live(), feed({ axis: 'y', direction: -1, extent: { kind: 'distance', distance: 12.5 } }))
    expect(distance.ok && distance.plan.text).toBe('G21 G90 G94 G53 G1 Y37.500 F300')
    const exact = powerFeedPlan(live(), feed({ axis: 'y', extent: { kind: 'distance', distance: 199.5 } }))
    expect(exact.ok && exact.plan.target).toBe(249.5)
    expect(powerFeedPlan(live(), feed({ axis: 'y', extent: { kind: 'distance', distance: 199.6 } })))
      .toEqual({ ok: false, reason: 'Y+ has 199.500 mm before the soft limit' })
    expect(powerFeedPlan(live(), feed({ extent: { kind: 'distance', distance: 0 } })))
      .toEqual({ ok: false, reason: 'Enter a distance above zero' })
  })
  it('refuses at the limit already', () => {
    const atLimit = live(moved(status(), { x: 399.6 }))
    expect(powerFeedPlan(atLimit, feed())).toEqual({ ok: false, reason: 'X is at its soft limit' })
    expect(powerFeedPlan(atLimit, feed({ extent: { kind: 'distance', distance: 1 } }))).toEqual({ ok: false, reason: 'X is at its soft limit' })
    expect(powerFeedPlan(atLimit, feed({ direction: -1 })).ok).toBe(true)
  })
  it('writes inch words when the interpreter is in G20, from millimetre targets', () => {
    const inch = powerFeedPlan(live(status({ program_units: 'inch' })), feed({ feed: 254 }))
    expect(inch.ok && inch.plan.text).toBe('G20 G90 G94 G53 G1 X15.7283 F10')
    const distance = powerFeedPlan(live(status({ program_units: 'inch' })), feed({ extent: { kind: 'distance', distance: 25.4 } }))
    expect(distance.ok && distance.plan.text).toBe('G20 G90 G94 G53 G1 X4.9370 F11.81')
  })
  it('refuses when off, busy, unhomed, without limits, with a probe, or at a bad feed', () => {
    expect(powerFeedPlan({ status: status(), machineBlock: 'Machine is off' }, feed())).toEqual({ ok: false, reason: 'Machine is off' })
    expect(powerFeedPlan({ status: status(), machineBlock: 'Program running' }, feed())).toEqual({ ok: false, reason: 'Program running' })
    const unhomed = status(); unhomed.axes = unhomed.axes.map((axis) => ({ ...axis, homed: axis.axis !== 'x' }))
    expect(powerFeedPlan(live(unhomed), feed())).toEqual({ ok: false, reason: 'Home X first' })
    expect(powerFeedPlan(live(unhomed), feed({ axis: 'y' })).ok).toBe(true)
    const unlimited = status(); unlimited.axes = unlimited.axes.map((axis) => ({ ...axis, min_limit: 0, max_limit: 0 }))
    expect(powerFeedPlan(live(unlimited), feed())).toEqual({ ok: false, reason: 'X has no soft limits' })
    expect(powerFeedPlan({ ...live(), probeLoaded: true }, feed())).toEqual({ ok: false, reason: 'A touch probe is loaded' })
    expect(powerFeedPlan(live(), feed({ feed: 0 }))).toEqual({ ok: false, reason: 'Enter a feed above zero' })
    expect(powerFeedPlan(live(), feed({ feed: 6001 }))).toEqual({ ok: false, reason: 'The feed is above the machine maximum' })
    expect(powerFeedPlan({ status: null, machineBlock: null }, feed())).toEqual({ ok: false, reason: 'No status from the machine yet' })
  })
})

describe('go to a saved position', () => {
  it('lifts Z to the safe height, moves X and Y, and lowers Z only when included', () => {
    expect(goToPlan(live(), [10, 20, -50], false)).toEqual({ ok: true, plan: { safeZ: -0.5, moves: [
      { target: { z: -0.5 }, text: 'G21 G90 G53 G0 Z-0.500' },
      { target: { x: 10, y: 20 }, text: 'G21 G90 G53 G0 X10.000 Y20.000' },
    ] } })
    const withZ = goToPlan(live(), [10, 20, -50], true)
    expect(withZ.ok && withZ.plan.moves.map((move) => move.text)).toEqual(['G21 G90 G53 G0 Z-0.500', 'G21 G90 G53 G0 X10.000 Y20.000', 'G21 G90 G53 G0 Z-50.000'])
  })
  it('keeps a Z already above the safe height and leaves out moves that would not move', () => {
    const high = goToPlan(live(moved(status(), { z: -0.2 })), [100, 50, -0.2], true)
    expect(high).toEqual({ ok: true, plan: { safeZ: -0.2, moves: [] } })
    const inch = goToPlan(live(status({ program_units: 'inch' })), [25.4, 50, -20], true)
    expect(inch.ok && inch.plan.moves.map((move) => move.text)).toEqual(['G20 G90 G53 G0 Z-0.0197', 'G20 G90 G53 G0 X1.0000 Y1.9685', 'G20 G90 G53 G0 Z-0.7874'])
  })
  it('lifts to the safe Z of the settings, or stays higher, and refuses one outside the Z limits', () => {
    const plan = goToPlan(live(), [10, 20, -50], true, -15)
    expect(plan.ok && plan.plan.safeZ).toBe(-15)
    expect(plan.ok && plan.plan.moves.map((move) => move.text)).toEqual(['G21 G90 G53 G0 Z-15.000', 'G21 G90 G53 G0 X10.000 Y20.000', 'G21 G90 G53 G0 Z-50.000'])
    const higher = goToPlan(live(moved(status(), { z: -5 })), [10, 20, -50], true, -15)
    expect(higher.ok && higher.plan.moves.map((move) => move.text)).toEqual(['G21 G90 G53 G0 X10.000 Y20.000', 'G21 G90 G53 G0 Z-50.000'])
    expect(goToPlan(live(), [10, 20, -50], true, 3)).toEqual({ ok: false, reason: 'The safe Z for Go to is outside the Z soft limits. Change it in Settings' })
    expect(safeZBlock(status(), -15, 'mm')).toBeNull()
    expect(safeZBlock(status(), -200.5, 'mm')).toBe('Safe Z must be within the Z soft limits, -200.000 to 0.000 mm')
    expect(safeZBlock(status(), 1, 'inch')).toBe('Safe Z must be within the Z soft limits, -7.8740 to 0.0000 in')
    expect(safeZBlock(null, 1, 'mm')).toBe('Z has no soft limits')
  })
  it('refuses a saved position outside the soft limits and an unhomed or busy machine', () => {
    expect(goToPlan(live(), [500, 20, -50], false)).toEqual({ ok: false, reason: 'The saved X is outside the soft limits' })
    expect(goToPlan(live(), [10, 20, -500], false).ok).toBe(true)
    expect(goToPlan(live(), [10, 20, -500], true)).toEqual({ ok: false, reason: 'The saved Z is outside the soft limits' })
    const unhomed = status(); unhomed.axes = unhomed.axes.map((axis) => ({ ...axis, homed: axis.axis !== 'z' }))
    expect(goToPlan(live(unhomed), [10, 20, -50], false)).toEqual({ ok: false, reason: 'Home Z first' })
    expect(goToPlan({ status: status(), machineBlock: 'Axes are not homed' }, [10, 20, -50], false)).toEqual({ ok: false, reason: 'Axes are not homed' })
    expect(moveBlock({ status: status(), machineBlock: 'MDI command running' }, ['x'])).toBe('MDI command running')
  })
  it('names new positions and limits how many are kept', () => {
    expect(nextPositionName([])).toBe('Position 1')
    expect(nextPositionName([{ name: 'Position 2', position: [0, 0, 0] }])).toBe('Position 3')
    expect(nextPositionName([{ name: 'A', position: [0, 0, 0] }, { name: 'Position 2', position: [0, 0, 0] }])).toBe('Position 3')
    expect(saveBlock(status(), Array.from({ length: 16 }, () => ({ name: 'A', position: [0, 0, 0] })))).toBe('At most 16 positions. Delete one first')
    const unhomed = status(); unhomed.axes = unhomed.axes.map((axis) => ({ ...axis, homed: false }))
    expect(saveBlock(unhomed, [])).toBe('Home X first')
    expect(saveBlock(status(), [])).toBeNull()
    // The measured position trails the command by the backlash of a linear scale.
    expect(arrived(moved(status(), { x: 10.125 }), { x: 10 })).toBe(true)
    expect(arrived(moved(status(), { x: 9.5 }), { x: 10 })).toBe(true)
    expect(arrived(moved(status(), { x: 10.51 }), { x: 10 })).toBe(false)
  })
})

describe('move sequence', () => {
  function sequenceRig(): { current: { value: Status }; sent: CommandBody[]; answer: (outcome?: CommandResult['outcome']) => void; block: { value: string | null }; sequence: ReturnType<typeof createMoveSequence> } {
    const current = shallowRef(status())
    const sent: CommandBody[] = []
    const listeners = new Set<ResultListener>()
    const block = { value: null as string | null }
    const sequence = createMoveSequence({
      send: (command): number => { sent.push(command); return sent.length },
      onResult: (listener): (() => void) => { listeners.add(listener); return (): void => { listeners.delete(listener) } },
      status: () => current.value,
      block: () => block.value,
    })
    const answer = (outcome: CommandResult['outcome'] = 'accepted'): void => {
      const command = sent.at(-1)
      if (command === undefined) throw new Error('Nothing sent')
      for (const listener of listeners) listener(command, { type: 'result', id: sent.length, outcome, reason: outcome === 'accepted' ? '' : 'Refused' })
    }
    return { current, sent, answer, block, sequence }
  }
  const plan = goToPlan(live(), [10, 20, -50], true)
  const moves = plan.ok ? plan.plan.moves : []

  it('sends each move only after the previous one arrived', () => {
    const r = sequenceRig()
    r.sequence.start(moves)
    expect(r.sent.map((command) => command.type === 'mdi' && command.text)).toEqual(['G21 G90 G53 G0 Z-0.500'])
    r.answer()
    r.current.value = moved(r.current.value, { z: -10 }, 'reading')
    r.current.value = moved(r.current.value, { z: -0.5 }, 'reading')
    expect(r.sent).toHaveLength(1)
    r.current.value = moved(r.current.value, { z: -0.5 })
    expect(r.sent).toHaveLength(2)
    r.answer(); r.current.value = moved(r.current.value, { x: 10, y: 20 })
    r.answer(); r.current.value = moved(r.current.value, { z: -50 })
    expect(r.sent.map((command) => command.type === 'mdi' && command.text)).toEqual(moves.map((move) => move.text))
    expect(r.sequence.state).toMatchObject({ running: false, error: '' })
  })
  it('waits for a short lift inside the feedback tolerance before moving X and Y', () => {
    const r = sequenceRig()
    r.current.value = moved(r.current.value, { z: -0.8 })
    r.sequence.start(moves)
    r.answer()
    expect(r.sent).toHaveLength(1)
    r.current.value = moved(r.current.value, { z: -0.8 })
    expect(r.sent).toHaveLength(1)
    r.current.value = { ...moved(r.current.value, { z: -0.62 }),
      motion: { ...r.current.value.motion, in_position: false } }
    expect(r.sent).toHaveLength(1)
    r.current.value = { ...moved(r.current.value, { z: -0.625 }),
      motion: { ...r.current.value.motion, in_position: true } }
    expect(r.sent).toHaveLength(2)
  })
  it('recognizes a short lift that completes before the next report can show motion', () => {
    const r = sequenceRig()
    r.current.value = moved(r.current.value, { z: -0.8 })
    r.sequence.start(moves)
    r.current.value = moved(r.current.value, { z: -0.5 })
    expect(r.sent).toHaveLength(1)
    r.answer()
    expect(r.sent).toHaveLength(2)
  })
  it('ends without another move when Stop leaves a move short, or a move is refused', () => {
    const r = sequenceRig()
    r.sequence.start(moves); r.answer()
    r.current.value = moved(r.current.value, { z: -10 }, 'reading')
    r.current.value = moved(r.current.value, { z: -8 })
    expect(r.sent).toHaveLength(1)
    expect(r.sequence.state).toMatchObject({ running: false, error: 'Go to stopped before the position was reached' })
    const refused = sequenceRig()
    refused.sequence.start(moves); refused.answer('rejected')
    expect(refused.sequence.state).toMatchObject({ running: false, error: 'Refused' })
    refused.current.value = moved(refused.current.value, { z: -0.5 })
    expect(refused.sent).toHaveLength(1)
  })
  it('ends on power loss or a move that never starts', () => {
    const r = sequenceRig()
    r.sequence.start(moves); r.answer()
    r.block.value = 'Machine is in E-stop'
    r.current.value = moved(r.current.value, { z: -0.5 })
    expect(r.sent).toHaveLength(1)
    expect(r.sequence.state.error).toBe('Machine is in E-stop')
    const idle = sequenceRig()
    idle.sequence.start(moves); idle.answer()
    for (let index = 0; index < 21; index += 1) idle.current.value = { ...idle.current.value, seq: index }
    expect(idle.sequence.state).toMatchObject({ running: false, error: 'The move did not start' })
    expect(idle.sent).toHaveLength(1)
    const disposed = sequenceRig()
    disposed.sequence.start(moves); disposed.answer(); disposed.sequence.dispose()
    disposed.current.value = moved(disposed.current.value, { z: -0.5 })
    expect(disposed.sent).toHaveLength(1)
  })
})
