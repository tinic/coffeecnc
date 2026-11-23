// Go to and power feed in the browser simulator under the interpreter state an operator has
// while cutting: a tool length offset, a work offset with rotation, a G92 shift, inch program
// units. The simulator must end a G53 move at the machine position given, as LinuxCNC does
// (backend/tests/sim/sim_tests.cpp, machine_moves), so the hosted demo behaves as the mill.
import { describe, expect, it } from 'vitest'

import { goToPlan, limitMarginMm, machinePosition, powerFeedPlan } from '../src/manual-moves'
import type { Status } from '../src/protocol'
import { rig, type Rig } from './harness'

function idle(r: Rig): Status {
  r.sim.advanceUntil((status) => status.interp === 'idle', 60_000)
  return r.status()
}
function mdi(r: Rig, text: string): Status {
  expect(r.send({ type: 'mdi', text }).outcome, text).toBe('accepted')
  return idle(r)
}
const near = (actual: readonly number[], expected: readonly number[]): boolean =>
  actual.every((value, index) => Math.abs(value - (expected[index] ?? NaN)) <= 0.01)

describe('Go to and power feed in the simulator', () => {
  it.each(['G21', 'G20'] as const)('end at the saved machine position with G43, G54 rotation and G92 active (%s)', (units) => {
    const r = rig()
    r.home()
    mdi(r, 'G21 G90 G0 X120 Y80 Z-40')
    mdi(r, 'G10 L2 P1 X12 Y-7 Z5 R30')
    mdi(r, 'G43 H1')
    mdi(r, 'G92 X1 Y2 Z3')
    mdi(r, `${units} G90`)
    const state = r.status()
    expect(state.offsets.tool[2]).toBe(62.5)
    expect(state.offsets.rotation_xy).toBe(30)
    expect(state.program_units).toBe(units === 'G20' ? 'inch' : 'mm')
    const saved = machinePosition(state)
    // Away from it, as with a jog.
    mdi(r, units === 'G20' ? 'G53 G0 X11.811 Y7.874 Z-4.724' : 'G53 G0 X300 Y200 Z-120')
    const plan = goToPlan({ status: r.status(), machineBlock: null }, saved, true)
    if (!plan.ok) throw new Error(plan.reason)
    expect(plan.plan.moves.map((move) => move.text.split(' ')[0])).toEqual([units, units, units])
    for (const move of plan.plan.moves) mdi(r, move.text)
    expect(near(machinePosition(r.status()), saved), `${String(machinePosition(r.status()))} vs ${String(saved)}`).toBe(true)
    // The tool offset and the program units are as the operator left them.
    expect(r.status().offsets.tool[2]).toBe(62.5)
    expect(r.status().program_units).toBe(units === 'G20' ? 'inch' : 'mm')
  })

  it('stops a power feed to the limit inside the soft limit with a tool length offset active', () => {
    const r = rig()
    r.home()
    mdi(r, 'G21 G90 G53 G0 Z-100')
    mdi(r, 'G43 H1')
    const feed = powerFeedPlan({ status: r.status(), machineBlock: null },
      { axis: 'z', direction: 1, extent: { kind: 'limit' }, feed: 6000, maxFeed: 6000, unit: 'mm' })
    if (!feed.ok) throw new Error(feed.reason)
    const z = mdi(r, feed.plan.text).axes.find((axis) => axis.axis === 'z')
    expect(z?.machine).toBeCloseTo((z?.max_limit ?? NaN) - limitMarginMm, 3)
    expect(r.errors()).toEqual([])
  })
})
