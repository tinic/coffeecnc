// The restart dialog keeps a started restart until the machine answers, and stops the machine
// when it gives up on an answer or the operator closes it.

import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { nextTick } from 'vue'

import RestartDialog from '../src/components/RestartDialog.vue'
import type { FromMachine, RestartPlan, Status, ToMachine } from '../src/protocol'
import { hello, SimMachine } from '../src/sim/machine'

const wire = vi.hoisted(() => ({
  receive: (message: FromMachine): void => { throw new Error(`No listener for ${message.type}`) },
  send: vi.fn<(command: ToMachine) => void>(),
}))
vi.mock('../src/machine', async () => {
  const { computed } = await import('vue')
  const { createPanel } = await import('../src/store')
  const instance = createPanel({ transport: { simulated: false, send: wire.send,
    listen: (listener): void => { wire.receive = listener } } })
  return { panel: instance.state, send: instance.send, onResult: instance.onResult,
    reported: computed(() => instance.state.status), findTool: (): null => null }
})

const plan: RestartPlan = {
  plan_id: 7, line: 14, tool: 2, needs_tool_change: false, wcs: 1, h: 2,
  spindle_direction: 'forward', spindle_rpm: 2000, flood: false, mist: false, feed: 480,
  units: 'mm', plane: 'XY', distance_mode: 'absolute', clearance_z: 30, start: [16, 12, 10],
  approach_feed: 300,
}

let wrapper: VueWrapper | undefined
function button(label: string): HTMLButtonElement {
  const found = wrapper?.findAll<HTMLButtonElement>('footer button').find((b) => b.text().includes(label))
  if (!found) throw new Error(`Missing button ${label}`)
  return found.element
}
function answer(type: ToMachine['type'], outcome: 'accepted' | 'timed_out', extra = {}): void {
  const command = wire.send.mock.calls.findLast(([c]) => c.type === type)?.[0]
  if (!command) throw new Error(`No ${type}`)
  wire.receive({ type: 'result', id: command.id, outcome, reason: '', ...extra })
}
async function started(): Promise<void> {
  wrapper = mount(RestartDialog, { props: { line: 14, section: 'T2' }, attachTo: document.body,
    global: { stubs: { teleport: true } } })
  answer('plan_restart', 'accepted', { restart: plan })
  await nextTick()
  button('Cycle start').click()
  await nextTick()
  expect(wire.send.mock.calls.at(-1)?.[0]).toMatchObject({ type: 'start_restart', plan_id: 7 })
}

beforeEach(() => {
  wire.send.mockReset()
  const status: Status = { ...new SimMachine({ emit: (): void => undefined }).status(), machine: 'on' }
  wire.receive({ ...hello, simulated: false })
  wire.receive(status)
})
afterEach(() => { wrapper?.unmount(); wrapper = undefined })

describe('restart dialog', () => {
  it('reviews the plan prepared at M6 without planning again from the setter position', async () => {
    wrapper = mount(RestartDialog, {
      props: { line: plan.line, section: '', prepared: plan }, attachTo: document.body,
      global: { stubs: { teleport: true } },
    })
    const status = new SimMachine({ emit: (): void => undefined }).status()
    wire.receive({ ...status, seq: 500, machine: 'on', preview_context: 3 })
    await nextTick()
    expect(wire.send).not.toHaveBeenCalled()
    expect(wrapper.text()).toContain('G43 H2')
    button('Cycle start').click()
    expect(wire.send.mock.calls.map(([command]) => command)).toEqual([
      expect.objectContaining({ type: 'start_restart', plan_id: plan.plan_id }),
    ])
  })

  it('stops the machine when the start is closed before its answer', async () => {
    await started()
    button('Stop').click()
    await nextTick()
    expect(wire.send.mock.calls.at(-1)?.[0].type).toBe('abort')
    expect(wrapper?.emitted('close')).toHaveLength(1)
  })

  it('stops the machine when the start has no answer, and Cancel then sends nothing more', async () => {
    await started()
    answer('start_restart', 'timed_out')
    await nextTick()
    expect(wire.send.mock.calls.map(([c]) => c.type)).toEqual(['plan_restart', 'start_restart', 'abort'])
    expect(wrapper?.text()).toContain('CoffeeCNC sent Stop')
    button('Cancel').click()
    expect(wire.send).toHaveBeenCalledTimes(3)
  })

  it('closes on an accepted start without sending Stop', async () => {
    await started()
    answer('start_restart', 'accepted')
    await nextTick()
    expect(wrapper?.emitted('close')).toHaveLength(1)
    expect(wire.send.mock.calls.map(([c]) => c.type)).toEqual(['plan_restart', 'start_restart'])
  })
})
