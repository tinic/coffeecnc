// Macros of the MDI card (#211): hidden without configuration, one confirmation that shows the
// block, the rules of a typed MDI block, and the result in the MDI history.
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { nextTick } from 'vue'
import ManualTab from '../src/components/ManualTab.vue'
import MdiMacros from '../src/components/MdiMacros.vue'
import { panel } from '../src/machine'
import type { FromMachine, Hello, Status, ToMachine } from '../src/protocol'
import type { Transport } from '../src/bridge'
import { hello, SimMachine } from '../src/sim/machine'

const wire = vi.hoisted(() => ({
  receive: (message: FromMachine): void => { throw new Error(`No listener for ${message.type}`) },
  send: vi.fn<(command: ToMachine) => void>(),
}))
vi.mock('../src/bridge', () => ({
  createTransport: (): Transport => ({ simulated: false, send: wire.send,
    listen: (listener: (message: FromMachine) => void): void => { wire.receive = listener } }),
}))

const macros = [
  { label: 'Park', mdi: 'G53 G0 Z0' },
  { label: 'Clamp check', mdi: 'O<clamp> call' },
]
let wrapper: VueWrapper | undefined
let status: Status

function peer(changes: Partial<Hello> = {}, configured: Hello['macros'] | null = macros): void {
  const own: Hello = { ...hello, simulated: false, ...changes }
  if (configured === null) delete own.macros
  else own.macros = configured
  wire.receive(own)
  wire.receive(status)
}
function view(component: typeof MdiMacros = MdiMacros): VueWrapper {
  wrapper = mount(component, { attachTo: document.body, global: { stubs: { teleport: true } } })
  return wrapper
}
function button(label: string): HTMLButtonElement {
  const found = wrapper?.findAll('button').find((b) => b.text() === label)
  if (!found) throw new Error(`Missing button ${label}`)
  return found.element
}
async function tap(label: string): Promise<void> {
  button(label).click()
  await nextTick()
}
const sent = (): ToMachine[] => wire.send.mock.calls.map(([command]) => command).filter((c) => c.type !== 'ready')

beforeEach(() => {
  wire.send.mockReset()
  // Jsdom has no scrolling; the MDI history scrolls to its end.
  Object.defineProperty(HTMLElement.prototype, 'scrollTo', { configurable: true, value: vi.fn() })
  status = new SimMachine({ emit: (): void => undefined }).status()
  status.machine = 'on'
  status.axes = status.axes.map((axis) => ({ ...axis, homed: true }))
  panel.mdi.splice(0)
})
afterEach(() => { wrapper?.unmount(); wrapper = undefined })

describe('MDI macros', () => {
  it('shows nothing when the machine has none', async () => {
    peer({}, null)
    const manual = view(ManualTab)
    await nextTick()
    expect(manual.find('.mdi').exists()).toBe(true)
    expect(manual.findAll('button').some((b) => b.text() === 'Macros')).toBe(false)
    peer({}, []); await nextTick()
    expect(manual.findAll('button').some((b) => b.text() === 'Macros')).toBe(false)
  })

  it('puts one Macros button on the MDI entry line', async () => {
    peer()
    const manual = view(ManualTab)
    await nextTick()
    const line = manual.get('.mdi form')
    expect(line.findAll('button').map((b) => b.text())).toEqual(['Macros', 'Run'])
    expect(manual.text()).not.toContain('Clamp check')
  })

  it('runs the block only after the confirmation, and records it in the MDI history', async () => {
    peer(); view()
    await tap('Macros')
    expect(wrapper?.findAll('.macro').map((b) => b.text())).toEqual(['Park', 'Clamp check'])
    await tap('Clamp check')
    expect(wrapper?.text()).toContain('Run Clamp check?')
    expect(wrapper?.get('.block').text()).toBe('O<clamp> call')
    expect(sent()).toEqual([])
    await tap('Run')
    expect(sent()).toEqual([expect.objectContaining({ type: 'mdi', text: 'O<clamp> call' })])
    expect(wrapper?.find('.block').exists()).toBe(false)
    const id = sent()[0]?.id ?? -1
    expect(panel.mdi.at(-1)).toMatchObject({ id, text: 'O<clamp> call', outcome: 'pending' })
    wire.receive({ type: 'result', id, outcome: 'rejected', reason: 'Wait for the interpreter and motion to become idle.' })
    await nextTick()
    expect(panel.mdi.at(-1)).toMatchObject({ outcome: 'rejected', reason: 'Wait for the interpreter and motion to become idle.' })
  })

  it('sends nothing when cancelled', async () => {
    peer(); view()
    await tap('Macros'); await tap('Park'); await tap('Cancel')
    expect(wrapper?.find('.block').exists()).toBe(false)
    expect(sent()).toEqual([])
  })

  it('invalidates the confirmation when the machine configuration changes', async () => {
    peer(); view()
    await tap('Macros'); await tap('Park')
    const oldRun = button('Run')
    peer({}, [{ label: 'Park', mdi: 'G53 G0 Z-20' }])
    oldRun.click()
    await nextTick()
    expect(sent()).toEqual([])
    expect(wrapper?.find('[role="dialog"]').exists()).toBe(false)
    await tap('Macros'); await tap('Park')
    expect(wrapper?.get('.block').text()).toBe('G53 G0 Z-20')
    await tap('Run')
    expect(sent()).toEqual([expect.objectContaining({ type: 'mdi', text: 'G53 G0 Z-20' })])
  })

  it.each([
    ['machine off', (s: Status): void => { s.machine = 'off' }, {}, 'Machine is off'],
    ['E-stop', (s: Status): void => { s.machine = 'estop' }, {}, 'Machine is in E-stop'],
    ['not homed', (s: Status): void => { s.axes = s.axes.map((axis) => ({ ...axis, homed: axis.axis !== 'z' })) }, { homing_required: true }, 'Axes are not homed'],
    ['a running program', (s: Status): void => { s.mode = 'auto'; s.interp = 'reading' }, {}, 'Program running'],
    ['a stale link', (s: Status): void => { s.link = 'stale' }, {}, 'LinuxCNC has stopped updating its status'],
  ] as const)('refuses with %s, like a typed block', async (_name, change, capabilities, reason) => {
    change(status); peer(capabilities); view()
    await tap('Macros'); await tap('Park')
    expect(wrapper?.get('.refusal').text()).toBe(reason)
    expect(button('Run').getAttribute('aria-disabled')).toBe('true')
    await tap('Run')
    expect(sent()).toEqual([])
    expect(panel.mdi).toEqual([])
  })

  it('runs unhomed where the machine does not require homing', async () => {
    status.axes = status.axes.map((axis) => ({ ...axis, homed: false }))
    peer({ homing_required: false }); view()
    await tap('Macros'); await tap('Park'); await tap('Run')
    expect(sent()).toEqual([expect.objectContaining({ type: 'mdi', text: 'G53 G0 Z0' })])
  })
})
