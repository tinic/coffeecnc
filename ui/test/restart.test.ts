// Restart from a line: the simulator plans the state at the line without motion, then runs the
// tool change, the approach and the rest of the program. The code list offers the line.

import { afterEach, describe, expect, it, vi } from 'vitest'
import { nextTick } from 'vue'
import { mount, type VueWrapper } from '@vue/test-utils'

import NcText from '../src/components/NcText.vue'
import type { RestartPlan } from '../src/protocol'
import { rig, work } from './harness'

// Two tool sections; line 14 is the first motion of the T2 section.
const program = [
  'G21 G90 G17 G54', //  1
  'T1 M6', //  2
  'G43 H1', //  3
  'G0 Z30', //  4
  'G0 X-16 Y-12', //  5
  'S1500 M3', //  6
  'G1 Z10 F600', //  7
  'G1 X16 F480', //  8
  'M5', //  9
  'G0 Z30', // 10
  'T2 M6', // 11
  'G43 H2', // 12
  'S2000 M3 M8', // 13
  'G0 X-16 Y12', // 14
  'G1 Z10 F600', // 15
  'G1 X16 F480', // 16
  'M5 M9', // 17
  'G0 Z30', // 18
  'M30', // 19
].join('\n')

function ready(): ReturnType<typeof rig> {
  const r = rig()
  r.home()
  r.send({ type: 'mdi', text: 'G10 L2 P1 X150 Y100 Z-150' })
  r.sim.advance(200)
  r.load(program)
  r.send({ type: 'run' })
  r.finish()
  r.sim.advance(400)
  return r
}

describe('restart in the simulator', () => {
  it('plans the T2 section from T1 without motion, then changes tool, approaches and finishes', () => {
    const r = ready()
    const before = r.status()
    expect(before.tool.in_spindle).toBe(2)
    // Put T1 back so the restart has to ask for T2.
    r.send({ type: 'mdi', text: 'T1 M6' })
    r.finish()
    r.sim.advance(400)
    const from = work(r.status())

    const planned = r.send({ type: 'plan_restart', line: 14, preview_context: r.status().preview_context })
    expect(planned.outcome).toBe('accepted')
    const plan = planned.restart
    if (plan === undefined) throw new Error('no restart plan')
    // G43 H2 with no move: the same machine Z reads 14.3 mm higher with the shorter T2.
    plan.start = plan.start.map((value) => Math.round(value * 1000) / 1000) as RestartPlan['start']
    plan.clearance_z = Math.round(plan.clearance_z * 1000) / 1000
    expect(plan).toMatchObject({
      line: 14, tool: 2, needs_tool_change: true, wcs: 1, h: 2,
      spindle_direction: 'forward', spindle_rpm: 2000, flood: true, mist: false, feed: 480,
      units: 'mm', plane: 'XY', distance_mode: 'absolute', clearance_z: 44.3, start: [16, -12, 44.3], approach_feed: 300,
    })
    r.sim.advance(200)
    expect(work(r.status())).toEqual(from)

    expect(r.send({ type: 'start_restart', plan_id: plan.plan_id }).outcome).toBe('accepted')
    r.sim.advanceUntil((status) => status.wait.kind === 'tool_change', 10_000)
    expect(r.status().wait).toMatchObject({ kind: 'tool_change', tool: 2 })
    // Lifted to clearance with the old tool before the change; nothing turns yet.
    expect(work(r.status())[2]).toBeCloseTo(44.3, 3)
    expect(r.status().spindle.direction).toBe('stopped')
    expect(r.status().coolant.flood).toBe(false)
    let lowest = Infinity
    for (let n = 0; n < 200_000 && r.status().interp !== 'idle'; n += 1) {
      const status = r.status()
      if (status.wait.kind === 'tool_change') r.send({ type: 'tool_changed' })
      lowest = Math.min(lowest, work(status)[2] ?? Infinity)
      r.sim.tick()
    }
    expect(r.errors()).toEqual([])
    expect(lowest).toBeCloseTo(10, 3)
    expect(r.status().tool.in_spindle).toBe(2)
    expect(work(r.status())).toEqual([16, 12, 30])
  })

  it('refuses a stale preview, a line outside the program and a plan made before a change', () => {
    const r = ready()
    const context = r.status().preview_context
    expect(r.send({ type: 'plan_restart', line: 14, preview_context: context - 1 })).toMatchObject({
      outcome: 'rejected', reason: 'Wait for the preview of this program',
    })
    expect(r.send({ type: 'plan_restart', line: 99, preview_context: context })).toMatchObject({
      outcome: 'rejected', reason: 'Line 99 is not in the program',
    })
    const plan = r.send({ type: 'plan_restart', line: 14, preview_context: context }).restart
    if (plan === undefined) throw new Error('no restart plan')
    r.load(program)
    expect(r.send({ type: 'start_restart', plan_id: plan.plan_id })).toMatchObject({
      outcome: 'rejected', reason: 'The program or machine changed since the restart was planned. Plan it again.',
    })
    expect(r.send({ type: 'start_restart', plan_id: plan.plan_id }).reason).toBe(
      'This restart plan is no longer valid. Plan it again.')
  })
})

describe('restart from the code list', () => {
  let wrapper: VueWrapper | null = null
  afterEach(() => {
    wrapper?.unmount()
    wrapper = null
    vi.unstubAllGlobals()
  })

  it('offers the tapped line only while restart is possible', async () => {
    Object.defineProperty(document, 'fonts', { configurable: true, value: { ready: Promise.resolve() } })
    vi.stubGlobal('ResizeObserver', class {
      observe(): void { /* Fixed size in the test. */ }
      disconnect(): void { /* Nothing observed. */ }
    })
    const lines = program.split('\n')
    const pane = mount(NcText, { props: { lines, motionLine: 0, currentLine: 0, errorLine: 0, restartable: false } })
    wrapper = pane
    await nextTick()
    await pane.findAll('.number')[2]?.trigger('click')
    expect(pane.find('.restart').exists()).toBe(false)

    await pane.setProps({ restartable: true })
    await pane.findAll('.number')[2]?.trigger('click')
    expect(pane.get('.restart').text()).toBe('Restart from line 3')
    expect(pane.findAll('.row.picked').map((row) => row.get('.number').text())).toEqual(['3'])
    await pane.get('.restart').trigger('click')
    expect(pane.emitted('restart')).toEqual([[3]])

    await pane.findAll('.number')[2]?.trigger('click')
    expect(pane.find('.restart').exists()).toBe(false)
  })
})
