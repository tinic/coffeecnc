// The HAL diagnostics view (#251): it asks for readings only while it is open, shows each row
// with words that fit its role and with what HAL says of its wiring, and starts the HAL tools.
// Also the simulator's readings and the menu entry.
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { nextTick } from 'vue'

import DiagnosticsDialog from '../src/components/DiagnosticsDialog.vue'
import { alarms, groups, shown } from '../src/diagnostics'
import { setLanguage } from '../src/i18n'
import type { DiagnosticInput, Diagnostics, FromMachine, ToMachine } from '../src/protocol'
import { diagnosticsMs, hello, SimMachine } from '../src/sim/machine'
import { rig } from './harness'

const wire = vi.hoisted(() => ({ receive: (message: FromMachine): void => { throw new Error(`No listener for ${message.type}`) }, send: vi.fn<(command: ToMachine) => void>() }))
vi.mock('../src/machine', async () => {
  const { computed } = await import('vue')
  const { createPanel } = await import('../src/store')
  const instance = createPanel({ transport: { simulated: false, send: wire.send,
    listen: (listener): void => { wire.receive = listener } } })
  return { panel: instance.state, send: instance.send, onResult: instance.onResult,
    live: computed(() => instance.state.link === 'live'), reported: computed(() => instance.state.status),
    dismissNotice: instance.dismissNotice, toggleTheme: (): void => undefined }
})

const pin = (input: Partial<DiagnosticInput> & Pick<DiagnosticInput, 'role' | 'name'>): DiagnosticInput =>
  ({ kind: 'pin', state: 'live', value: false, ...input })

/** The G0759 as its INI's DIAGNOSTIC lines show it (docs/machines/g0759.md), at rest. */
function g0759(changes: Record<string, Partial<DiagnosticInput>> = {}): Diagnostics {
  const inputs: DiagnosticInput[] = []
  for (const [joint, axis] of ['x', 'y', 'z'].entries()) {
    inputs.push(
      pin({ role: 'min_limit', joint, axis, name: `joint.${String(joint)}.neg-lim-sw-in`, state: 'undriven' }),
      pin({ role: 'max_limit', joint, axis, name: `joint.${String(joint)}.pos-lim-sw-in`, state: 'undriven' }),
      pin({ role: 'home_switch', joint, axis, name: `joint.${String(joint)}.home-sw-in` }),
      pin({ role: 'amp_fault', joint, axis, name: `joint.${String(joint)}.amp-fault-in` }),
    )
  }
  inputs.push(
    pin({ role: 'estop_chain', name: 'iocontrol.0.emc-enable-in', value: true }),
    pin({ role: 'estop_request', name: 'iocontrol.0.user-enable-out', value: true }),
    pin({ role: 'probe', name: 'motion.probe-input' }),
    pin({ role: 'spindle_on', name: 'spindle.0.on' }),
    pin({ role: 'spindle_at_speed', name: 'spindle.0.at-speed', value: true }),
    pin({ role: 'spindle_speed_out', name: 'spindle.0.speed-out', value: 0 }),
    pin({ role: 'spindle_speed_in', name: 'spindle.0.speed-in', value: 0, state: 'unwired' }),
    pin({ role: 'spindle_fault', name: 'spindle.0.amp-fault-in', state: 'unwired' }),
    pin({ role: 'configured', label: 'E-stop button (input-16)', name: 'estop-ext', kind: 'signal', value: true }),
    pin({ role: 'configured', label: 'Setter contact (input-19)', name: 'setter-tripped', kind: 'signal' }),
    pin({ role: 'configured', label: 'VFD Modbus errors', name: 'svd-ps_vfd.modbus-errors', value: 2 }),
    pin({ role: 'configured', label: 'Spare', name: 'spare-in', state: 'missing', value: null }),
  )
  return { type: 'diagnostics',
    inputs: inputs.map((input) => ({ ...input, ...changes[input.name] })),
    tools: [{ tool: 'halshow', available: true, open: false }, { tool: 'halmeter', available: true, open: true },
      { tool: 'halscope', available: false, open: false }] }
}

let wrapper: VueWrapper | undefined
function open(): VueWrapper {
  wrapper = mount(DiagnosticsDialog, { attachTo: document.body, global: { stubs: { teleport: true } } })
  return wrapper
}
function sent(): ToMachine[] {
  return wire.send.mock.calls.map(([command]) => command)
}
function row(name: string): string {
  const found = wrapper?.findAll('li').find((element) => element.text().includes(name))
  return found?.text() ?? ''
}
function tool(name: string): HTMLButtonElement {
  const found = wrapper?.findAll('button.tool').find((button) => button.text().includes(name))
  if (!found) throw new Error(`Missing ${name}`)
  return found.element as HTMLButtonElement
}

beforeEach(async () => {
  vi.useFakeTimers()
  wire.send.mockReset()
  wire.receive({ ...hello, simulated: false })
  wire.receive(new SimMachine({ emit: (): void => undefined }).status())
  await setLanguage('en')
})
afterEach(() => {
  wrapper?.unmount()
  wrapper = undefined
  vi.useRealTimers()
})

describe('what a reading shows', () => {
  it('words a bit by the role of its row, and dims what nothing drives', () => {
    expect(shown(pin({ role: 'min_limit', name: 'a', value: true }))).toMatchObject({ key: 'status.diagnostics.value.tripped', tone: 'stop', note: null })
    expect(shown(pin({ role: 'min_limit', name: 'a', value: false }))).toMatchObject({ key: 'status.diagnostics.value.clear', tone: 'go' })
    expect(shown(pin({ role: 'estop_chain', name: 'a', value: false }))).toMatchObject({ key: 'status.diagnostics.value.open', tone: 'stop' })
    expect(shown(pin({ role: 'estop_request', name: 'a', value: false }))).toMatchObject({ key: 'status.diagnostics.value.estop', tone: 'stop' })
    expect(shown(pin({ role: 'probe', name: 'a', value: true }))).toMatchObject({ key: 'status.diagnostics.value.tripped', tone: 'warn' })
    expect(shown(pin({ role: 'amp_fault', name: 'a', value: true }))).toMatchObject({ key: 'status.diagnostics.value.fault', tone: 'stop' })
    // A limit on a signal nothing writes is no switch at all: neither safe nor tripped.
    expect(shown(pin({ role: 'max_limit', name: 'a', value: false, state: 'undriven' })))
      .toMatchObject({ key: 'status.diagnostics.value.clear', tone: 'idle', note: 'status.diagnostics.state.undriven' })
    expect(shown(pin({ role: 'configured', name: 'a', state: 'missing', value: null })))
      .toMatchObject({ key: 'status.diagnostics.value.none', note: 'status.diagnostics.state.missing' })
  })
  it('shows both spindle speeds in rpm and other numbers as they are', () => {
    expect(shown(pin({ role: 'spindle_speed_in', name: 'a', value: 20.5 })).text).toBe('1230 rpm')
    expect(shown(pin({ role: 'spindle_speed_out', name: 'a', value: 1200.4 })).text).toBe('1200 rpm')
    expect(shown(pin({ role: 'configured', name: 'a', value: 0.12345 })).text).toBe('0.123')
  })
  it('groups the joints with their axis, and counts what needs attention', () => {
    const reading = g0759({ 'joint.2.pos-lim-sw-in': { state: 'live', value: true } })
    const sections = groups(reading)
    expect(sections.joints.map((joint) => [joint.joint, joint.axis])).toEqual([[0, 'x'], [1, 'y'], [2, 'z']])
    expect(sections.joints[2]?.max?.value).toBe(true)
    expect(sections.estop.map((input) => input.role)).toEqual(['estop_chain', 'estop_request'])
    expect(sections.spindle).toHaveLength(5)
    expect(sections.configured.map((input) => input.label)).toEqual(['E-stop button (input-16)', 'Setter contact (input-19)', 'VFD Modbus errors', 'Spare'])
    expect(alarms(reading)).toBe(1)
    expect(alarms(g0759())).toBe(0)
  })
})

describe('diagnostics view', () => {
  it('watches while open, and only then', () => {
    open()
    expect(sent()).toEqual([{ type: 'watch_diagnostics', on: true, id: expect.any(Number) as number }])
    expect(wrapper?.text()).toContain('Waiting for the first reading')
    wrapper?.unmount(); wrapper = undefined
    expect(sent().at(-1)).toMatchObject({ type: 'watch_diagnostics', on: false })
  })
  it('shows the limit, home, E-stop, probe and spindle rows live', async () => {
    open()
    wire.receive(g0759()); await nextTick()
    expect(wrapper?.text()).toContain('Live')
    expect(wrapper?.findAll('tbody tr')).toHaveLength(3)
    expect(wrapper?.find('tbody tr').text()).toMatch(/^X\s*joint 0/)
    expect(row('External E-stop chain')).toContain('Closed')
    expect(row('LinuxCNC enable')).toContain('Enabled')
    expect(row('Probe input')).toContain('Clear')
    expect(row('Measured speed')).toContain('Not wired')
    // A limit switch the machine does not have: its signal has no source.
    expect(wrapper?.find('tbody tr td').text()).toContain('No source')

    wire.receive(g0759({ 'motion.probe-input': { value: true }, 'iocontrol.0.emc-enable-in': { value: false },
      'iocontrol.0.user-enable-out': { value: false }, 'setter-tripped': { value: true } })); await nextTick()
    expect(row('Probe input')).toContain('Tripped')
    expect(row('External E-stop chain')).toContain('Open')
    expect(row('LinuxCNC enable')).toContain('E-stop')
    expect(row('Setter contact (input-19)')).toContain('On')
    expect(wrapper?.text()).toContain('2 inputs need attention')
  })
  it('shows the configured rows with their names, and what HAL does not have', async () => {
    open()
    wire.receive(g0759()); await nextTick()
    expect(row('VFD Modbus errors')).toContain('svd-ps_vfd.modbus-errors')
    expect(row('VFD Modbus errors')).toContain('2')
    expect(row('Spare')).toContain('Not in HAL')
    wire.receive({ ...g0759(), inputs: g0759().inputs.filter((input) => input.role !== 'configured') }); await nextTick()
    expect(wrapper?.text()).toContain('None configured. Add one line per signal to [COFFEECNC] in the INI file, for example: DIAGNOSTIC = E-stop button | estop-ext')
  })
  it('says when the readings stop coming', async () => {
    open()
    wire.receive(g0759()); await nextTick()
    vi.advanceTimersByTime(1500); await nextTick()
    expect(wrapper?.text()).toContain('No reading for a second: the values are not current.')
    wire.receive(g0759()); await nextTick()
    vi.advanceTimersByTime(300); await nextTick()
    expect(wrapper?.text()).not.toContain('No reading for a second')
  })
  it('starts a HAL tool, and shows what is missing, open or refused', async () => {
    open()
    wire.receive(g0759()); await nextTick()
    expect(tool('halscope').disabled).toBe(true)
    expect(tool('halscope').textContent).toContain('Not installed')
    expect(tool('halmeter').disabled).toBe(true)
    expect(tool('halmeter').textContent).toContain('Already open')
    tool('halshow').click(); await nextTick()
    const launch = sent().at(-1)
    expect(launch).toMatchObject({ type: 'launch_hal_tool', tool: 'halshow' })
    expect(tool('halshow').textContent).toContain('Starting')
    wire.receive({ type: 'result', id: launch?.id ?? 0, outcome: 'rejected', reason: 'halshow is already open.' }); await nextTick()
    expect(wrapper?.find('.refusal').text()).toBe('halshow is already open.')
    expect(tool('halshow').disabled).toBe(false)
  })
  it('speaks every language', async () => {
    await setLanguage('de')
    open()
    wire.receive(g0759({ 'iocontrol.0.emc-enable-in': { value: false } })); await nextTick()
    expect(wrapper?.text()).toContain('HAL-Diagnose')
    expect(row('Externer Not-Halt-Kreis')).toContain('Offen')
    expect(wrapper?.text()).toContain('1 Eingang beachten')
    await setLanguage('ja'); await nextTick()
    expect(row('外部非常停止回路')).toContain('開')
  })
})

describe('simulator diagnostics', () => {
  it('reads only while watched, about ten times a second', () => {
    const r = rig()
    r.send({ type: 'ready' })
    expect(r.out.find((message) => message.type === 'hello')).toMatchObject({ diagnostics: true })
    r.sim.advance(500)
    expect(r.out.some((message) => message.type === 'diagnostics')).toBe(false)
    expect(r.send({ type: 'watch_diagnostics', on: true }).outcome).toBe('accepted')
    const from = r.out.length
    r.sim.advance(1000)
    const readings = r.out.slice(from).filter((message) => message.type === 'diagnostics')
    expect(readings.length).toBeGreaterThanOrEqual(1000 / diagnosticsMs - 1)
    expect(readings.length).toBeLessThanOrEqual(1000 / diagnosticsMs + 1)
    r.send({ type: 'watch_diagnostics', on: false })
    const after = r.out.length
    r.sim.advance(500)
    expect(r.out.slice(after).some((message) => message.type === 'diagnostics')).toBe(false)
  })
  it('follows E-stop, the spindle and homing plausibly', () => {
    const r = rig()
    r.send({ type: 'ready' })
    let reading = r.sim.diagnostics()
    const value = (name: string): DiagnosticInput['value'] | undefined => reading.inputs.find((input) => input.name === name)?.value
    expect(value('iocontrol.0.user-enable-out')).toBe(false)
    expect(value('iocontrol.0.emc-enable-in')).toBe(true)
    r.home()
    r.send({ type: 'set_spindle', direction: 'forward', rpm: 1200 })
    reading = r.sim.diagnostics()
    expect(value('iocontrol.0.user-enable-out')).toBe(true)
    expect(value('joint.0.home-sw-in')).toBe(true)
    expect(value('spindle.0.on')).toBe(true)
    expect(value('spindle.0.speed-out')).toBe(1200)
    expect(groups(reading).configured).toHaveLength(3)
    expect(reading.tools.every((halTool) => !halTool.available)).toBe(true)
    expect(r.send({ type: 'launch_hal_tool', tool: 'halscope' })).toMatchObject({ outcome: 'rejected',
      reason: 'The browser simulator has no HAL tools: it runs without LinuxCNC.' })
  })
})

describe('menu entry', () => {
  it('is offered only by a machine that serves diagnostics', async () => {
    const StatusBar = (await import('../src/components/StatusBar.vue')).default
    const bar = mount(StatusBar, { attachTo: document.body, global: { stubs: { teleport: true } } })
    expect(bar.findAll('button').some((button) => button.text() === 'Diagnostics')).toBe(true)
    bar.findAll('button').find((button) => button.text() === 'Diagnostics')?.element.click(); await nextTick()
    expect(bar.emitted('diagnostics')).toHaveLength(1)
    const older = { ...hello, simulated: false }
    delete older.diagnostics
    wire.receive(older); await nextTick()
    expect(bar.findAll('button').some((button) => button.text() === 'Diagnostics')).toBe(false)
    bar.unmount()
  })
})
