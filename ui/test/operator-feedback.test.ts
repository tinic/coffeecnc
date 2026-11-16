import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { nextTick } from 'vue'
import StatusBar from '../src/components/StatusBar.vue'
import { panel } from '../src/machine'
import type { FromMachine, Status, ToMachine } from '../src/protocol'
import { hello, SimMachine } from '../src/sim/machine'

const wire = vi.hoisted(() => ({ receive: (message: FromMachine): void => { throw new Error(`No listener for ${message.type}`) }, send: vi.fn<(message: ToMachine) => void>() }))
vi.mock('../src/machine', async () => {
  const { computed } = await import('vue')
  const { createPanel } = await import('../src/store')
  const instance = createPanel({ transport: { simulated: false, send: wire.send,
    listen: (listener): void => { wire.receive = listener } } })
  return { panel: instance.state, send: instance.send, onResult: instance.onResult, dismissNotice: instance.dismissNotice,
    reported: computed(() => instance.state.status), live: computed(() => instance.state.link === 'live') }
})
let wrapper: VueWrapper
let status: Status
function button(label: string): ReturnType<VueWrapper['get']> {
  const found = wrapper.findAll('button').find((b) => b.text() === label)
  if (found === undefined) throw new Error(`Missing ${label}`)
  return found
}
function result(outcome: 'accepted' | 'failed'): void {
  const command = wire.send.mock.calls.at(-1)?.[0]
  if (command === undefined) throw new Error('Missing command')
  wire.receive({ type: 'result', id: command.id, outcome, reason: outcome === 'failed' ? 'Power request failed' : '' })
}
function snapshot(machine: Status['machine'], link: Status['link'] = 'live'): void {
  wire.receive({ ...status, machine, link })
}
function isOpen(): boolean { return (wrapper.get('details').element as HTMLDetailsElement).open }
beforeEach(async () => {
  wire.send.mockReset()
  status = new SimMachine({ emit: (): void => undefined }).status()
  wire.receive({ ...hello }); snapshot('off')
  panel.notice = null; panel.log = []
  wrapper = mount(StatusBar, { attachTo: document.body })
  ;(wrapper.get('details').element as HTMLDetailsElement).open = true
  await wrapper.get('details').trigger('toggle')
})
afterEach(() => { wrapper.unmount() })

describe('power menu confirmation', () => {
  it.each(['status first', 'result first'])('closes only after accepted and fresh matching state: %s', async (order) => {
    await button('Machine on').trigger('click')
    expect(isOpen()).toBe(true)
    if (order === 'status first') snapshot('on'); else result('accepted')
    await nextTick(); expect(isOpen()).toBe(true)
    if (order === 'status first') result('accepted'); else snapshot('on')
    await nextTick(); expect(isOpen()).toBe(false)
  })
  it('handles synchronous simulator replies and a subsequent snapshot', async () => {
    wire.send.mockImplementation((command) => {
      wire.receive({ type: 'result', id: command.id, outcome: 'accepted', reason: '' })
      snapshot('on')
    })
    await button('Machine on').trigger('click')
    expect(isOpen()).toBe(false)
  })
  it('keeps a failed request visible even if the reported state subsequently matches', async () => {
    await button('Machine on').trigger('click')
    result('failed'); snapshot('on'); await nextTick()
    expect(isOpen()).toBe(true)
    expect(wrapper.get('[role="alert"]').text()).toContain('Power request failed')
  })
  it.each(['stale', 'hello'])('does not close on confirmation spanning %s', async (change) => {
    await button('Machine on').trigger('click')
    if (change === 'stale') snapshot('on', 'stale'); else wire.receive({ ...hello })
    await nextTick()
    result('accepted'); snapshot('on'); await nextTick()
    expect(isOpen()).toBe(true)
  })
  it('resets E-stop without powering on, then offers power-on only after confirmation', async () => {
    snapshot('estop'); await nextTick()
    await button('Reset E-stop').trigger('click'); result('accepted'); await nextTick()
    expect(isOpen()).toBe(true)
    snapshot('off'); await nextTick(); expect(isOpen()).toBe(false)
    expect(wire.send.mock.calls.map(([c]) => c.type === 'set_machine' && c.state)).toEqual(['off'])
    expect(button('Machine on').attributes('disabled')).toBeUndefined()
    expect(wrapper.text()).not.toContain('Reset E-stop')
  })
  it('keeps the direct power action pending when the menu is closed', async () => {
    await button('Machine on').trigger('click')
    ;(wrapper.get('details').element as HTMLDetailsElement).open = false
    await wrapper.get('details').trigger('toggle')
    await button('Machine on').trigger('click')
    expect(wire.send).toHaveBeenCalledTimes(1)
    result('accepted'); snapshot('on'); await nextTick()
    expect(button('Machine off').attributes('disabled')).toBeUndefined()
  })
})

it('keeps an error visible when later informational messages arrive, until dismissed', async () => {
  wire.receive({ type: 'message', level: 'error', text: 'Following error', time: 1 })
  wire.receive({ type: 'message', level: 'info', text: 'Program loaded', time: 2 })
  await nextTick()
  expect(wrapper.get('[role="alert"]').text()).toContain('Following error')
  expect(panel.log.map((entry) => entry.text)).toEqual(['Following error', 'Program loaded'])
  await button('Dismiss').trigger('click')
  expect(wrapper.get('[role="alert"]').text()).toBe('')
  expect(wrapper.find('.message-area').exists()).toBe(true)
})
