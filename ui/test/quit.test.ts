import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { nextTick } from 'vue'
import QuitDialog from '../src/components/QuitDialog.vue'
import { setLanguage } from '../src/i18n'
import { panel } from '../src/machine'
import type { FromMachine, RefusalMessage, Status, ToMachine } from '../src/protocol'
import { hello, SimMachine } from '../src/sim/machine'
import { rig } from './harness'

const wire = vi.hoisted(() => ({ receive: (message: FromMachine): void => { throw new Error(`No listener for ${message.type}`) }, send: vi.fn<(command: ToMachine) => void>() }))
vi.mock('../src/machine', async () => {
  const { computed } = await import('vue')
  const { createPanel } = await import('../src/store')
  const instance = createPanel({ transport: { simulated: false, send: wire.send,
    listen: (listener): void => { wire.receive = listener } } })
  return { panel: instance.state, send: instance.send, onResult: instance.onResult,
    live: computed(() => instance.state.link === 'live') }
})
let wrapper: VueWrapper | undefined
let status: Status
function open(): void {
  wrapper = mount(QuitDialog, { attachTo: document.body, global: { stubs: { teleport: true } } })
}
function button(label: string): HTMLButtonElement {
  const found = wrapper?.findAll('button').find(b => b.text() === label)
  if (!found) throw new Error(`Missing button ${label}`)
  return found.element
}
function result(outcome: 'accepted' | 'rejected' | 'failed' | 'timed_out', reason = '', refusal?: RefusalMessage): void {
  const command = wire.send.mock.calls.at(-1)?.[0]
  if (!command) throw new Error('No command')
  wire.receive({ type: 'result', id: command.id, outcome, reason, ...(refusal === undefined ? {} : { refusal }) })
}
beforeEach(() => {
  wire.send.mockReset()
  status = new SimMachine({ emit: (): void => undefined }).status()
  wire.receive({ ...hello, simulated: false })
  wire.receive(status)
  panel.notice = null
  panel.simulated = false
})
afterEach(() => { wrapper?.unmount(); wrapper = undefined })

describe('quit confirmation', () => {
  it('focuses Cancel and sends nothing when dismissed', async () => {
    open()
    expect(document.activeElement).toBe(button('Cancel'))
    button('Cancel').click(); await nextTick()
    expect(wrapper?.emitted('close')).toHaveLength(1)
    expect(wire.send).not.toHaveBeenCalled()
  })
  it.each([undefined, false])('does not send quit to an unsupported peer (%s)', async (supported) => {
    const peer = { ...hello }
    if (supported === undefined) delete peer.quit_supported
    else peer.quit_supported = supported
    wire.receive(peer); open()
    expect(button('Quit CoffeeCNC').disabled).toBe(true)
    button('Quit CoffeeCNC').click(); await nextTick()
    expect(wire.send).not.toHaveBeenCalled()
    expect(wrapper?.text()).toContain('does not support quitting')
  })
  it.each(['reading', 'paused', 'waiting'] as const)('blocks %s even when it starts after opening', async (interp) => {
    open(); wire.receive({ ...status, interp }); await nextTick()
    expect(button('Quit CoffeeCNC').disabled).toBe(true)
    expect(wrapper?.text()).toContain('Stop the program before quitting.')
    button('Quit CoffeeCNC').click(); expect(wire.send).not.toHaveBeenCalled()
  })
  it.each(['stale', 'down'] as const)('blocks a %s connection', async (link) => {
    open(); wire.receive({ ...status, link }); await nextTick()
    expect(button('Quit CoffeeCNC').disabled).toBe(true)
  })
  it('requires a fresh dialog after reconnect', async () => {
    open(); wire.receive({ ...hello }); wire.receive(status); await nextTick()
    expect(button('Quit CoffeeCNC').disabled).toBe(true)
    expect(wrapper?.text()).toContain('connection changed')
  })
  it.each(['on', 'off', 'estop'] as const)('permits idle %s without a separate power command', async (machine) => {
    wire.receive({ ...status, machine }); open()
    button('Quit CoffeeCNC').click(); await nextTick()
    expect(wire.send.mock.calls.map(([c]) => c.type)).toEqual(['quit'])
    expect(Object.keys(wire.send.mock.calls[0]?.[0] ?? {}).sort()).toEqual(['id', 'type'])
    expect(button('Quit CoffeeCNC').disabled).toBe(true)
    button('Quit CoffeeCNC').click(); expect(wire.send).toHaveBeenCalledTimes(1)
    result('accepted'); await nextTick()
    expect(wrapper?.text()).toContain('Quit request accepted. Waiting for CoffeeCNC to close')
    expect(wrapper?.text()).not.toContain('completed')
  })
  it.each(['rejected', 'failed', 'timed_out'] as const)('shows %s without claiming shutdown or retrying', async (outcome) => {
    open(); button('Quit CoffeeCNC').click()
    result(outcome, 'Switch-off was not confirmed'); await nextTick()
    expect(wrapper?.get('[role="alert"]').text()).toBe('Switch-off was not confirmed')
    expect(wire.send).toHaveBeenCalledTimes(1)
    expect(button('Cancel').disabled).toBe(false)
  })
  it('updates an existing refusal when the panel language changes', async () => {
    open(); button('Quit CoffeeCNC').click()
    result('rejected', 'The old fallback.', { key: 'messages.policy.stopBeforeQuit', params: {} })
    await nextTick()
    expect(wrapper?.get('[role="alert"]').text()).toBe('Stop the program before quitting.')
    await setLanguage('de')
    await nextTick()
    expect(wrapper?.get('[role="alert"]').text()).toBe('Stoppen Sie das Programm vor dem Beenden.')
  })
  it('handles a synchronous simulator response distinctly', async () => {
    wire.receive({ ...hello }); wire.receive(status)
    panel.simulated = true
    wire.send.mockImplementation((command) => { wire.receive({ type: 'result', id: command.id, outcome: 'accepted', reason: '' }) })
    open(); button('Quit CoffeeCNC').click(); await nextTick()
    expect(wrapper?.text()).toContain('Simulated quit completed. This browser stays open.')
    expect(button('Quit CoffeeCNC').disabled).toBe(true)
  })
  it('does not treat a native demo backend as a browser simulator', async () => {
    wire.receive({ ...hello, simulated: true }); wire.receive(status)
    open(); button('Quit CoffeeCNC').click(); result('accepted'); await nextTick()
    expect(wrapper?.text()).toContain('Waiting for CoffeeCNC to close')
    expect(wrapper?.text()).not.toContain('browser stays open')
  })
})

describe('simulated shutdown', () => {
  it('stops spindle/coolant and powers off while idle, preserving the browser', () => {
    const r = rig(); r.home()
    r.send({ type: 'set_spindle', direction: 'forward', rpm: 1000 })
    r.send({ type: 'set_coolant', flood: true, mist: true })
    expect(r.send({ type: 'quit' }).outcome).toBe('accepted')
    expect(r.status()).toMatchObject({ machine: 'off', spindle: { direction: 'stopped' }, coolant: { flood: false, mist: false } })
    expect(r.messages().at(-1)?.text).toContain('this browser stays open')
  })
  it('keeps E-stop asserted', () => {
    const r = rig(); expect(r.send({ type: 'quit' }).outcome).toBe('accepted')
    expect(r.status().machine).toBe('estop')
  })
  it.each(['running', 'paused', 'mdi'] as const)('refuses %s without aborting execution', (scene) => {
    const r = rig(); r.home()
    if (scene === 'mdi') r.send({ type: 'mdi', text: 'G1 X50 F100' })
    else { r.load('G1 X100 F100\nM2'); r.send({ type: 'run' }); if (scene === 'paused') r.send({ type: 'pause' }) }
    const before = r.status().interp
    expect(r.send({ type: 'quit' })).toMatchObject({ outcome: 'rejected', reason: 'Stop the program before quitting.',
      refusal: { key: 'messages.policy.stopBeforeQuit', params: {} } })
    expect(r.status().interp).toBe(before)
    expect(r.status().machine).toBe('on')
  })
})
