// The New operation dialog, the simulator's save_program and the drilling cycles the generated
// hole patterns use.

import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { nextTick } from 'vue'

import OperationDialog from '../src/components/OperationDialog.vue'
import { panel } from '../src/machine'
import type { FromMachine, ToMachine } from '../src/protocol'
import { programDirectory } from '../src/sim/files'
import { interpretLine, newModal, type Op } from '../src/sim/gcode'
import { hello, SimMachine } from '../src/sim/machine'
import { rig } from './harness'

const wire = vi.hoisted(() => ({ receive: (message: FromMachine): void => { throw new Error(`No listener for ${message.type}`) }, send: vi.fn<(command: ToMachine) => void>() }))
vi.mock('../src/machine', async () => {
  const { createPanel } = await import('../src/store')
  const instance = createPanel({ transport: { simulated: false, send: wire.send,
    listen: (listener): void => { wire.receive = listener } } })
  return { panel: instance.state, send: instance.send, onResult: instance.onResult }
})

let wrapper: VueWrapper | undefined
function sent(): ToMachine[] {
  return wire.send.mock.calls.map(([command]) => command)
}
function answer(command: ToMachine | undefined, outcome: 'accepted' | 'failed' = 'accepted', reason = ''): void {
  if (command === undefined) throw new Error('No command to answer')
  wire.receive({ type: 'result', id: command.id, outcome, reason })
}
function button(text: string): HTMLButtonElement {
  const found = wrapper?.findAll('button').find((candidate) => candidate.text() === text)
  if (!found) throw new Error(`Missing button ${text}`)
  return found.element
}
function input(label: string): HTMLInputElement {
  const found = wrapper?.findAll('input').find((candidate) => candidate.attributes('aria-label') === label)
  if (!found?.exists()) throw new Error(`Missing field ${label}`)
  return found.element
}
/** Opens the dialog and answers its listing of the program directory. */
async function open(existing: string[] = []): Promise<void> {
  wrapper = mount(OperationDialog, { attachTo: document.body, global: { stubs: { teleport: true } } })
  const listing = sent().at(-1)
  expect(listing).toMatchObject({ type: 'list_directory', path: null })
  wire.receive({ type: 'directory', path: programDirectory, parent: null,
    entries: existing.map((name) => ({ name, kind: 'file', bytes: 10, modified: 0 })) })
  answer(listing)
  await nextTick()
}

beforeEach(() => {
  wire.send.mockReset()
  wire.receive({ ...hello, simulated: false })
  wire.receive(new SimMachine({ emit: (): void => undefined }).status())
  wire.receive({ type: 'tool_table', revision: 1, tools: [
    { number: 1, pocket: 1, length: 60, diameter: 50, comment: 'Face mill 50 mm' },
    { number: 2, pocket: 2, length: 40, diameter: 6, comment: 'End mill 6 mm' },
    { number: 3, pocket: 3, length: 70, diameter: 5, comment: 'Drill 5 mm' },
  ] })
  panel.unit = 'mm'
})
afterEach(() => { wrapper?.unmount(); wrapper = undefined })

describe('new operation dialog', () => {
  it('saves the program under a free name, then opens it', async () => {
    await open(['facing-100x60mm.ngc'])
    expect(wrapper?.text()).toContain('facing-100x60mm-2.ngc')
    button('Create and open').click()
    await nextTick()
    const save = sent().at(-1)
    expect(save).toMatchObject({ type: 'save_program', path: `${programDirectory}/facing-100x60mm-2.ngc` })
    if (save?.type !== 'save_program') throw new Error('No save')
    expect(save.text).toContain('(Facing, made by CoffeeCNC)')
    expect(save.text).toContain('T1 M6\nG43 H1\n')
    expect(button('Saving…').disabled).toBe(true)
    answer(save)
    await nextTick()
    expect(sent().at(-1)).toMatchObject({ type: 'open_program', path: `${programDirectory}/facing-100x60mm-2.ngc` })
    expect(wrapper?.emitted('close')).toHaveLength(1)
  })

  it('keeps the dialog with the reason when the save fails, and opens nothing', async () => {
    await open()
    button('Create and open').click()
    answer(sent().at(-1), 'failed', 'A program named facing-100x60mm.ngc exists already.')
    await nextTick()
    expect(wrapper?.text()).toContain('exists already')
    expect(sent().map((command) => command.type)).toEqual(['list_directory', 'save_program'])
    expect(wrapper?.emitted('close')).toBeUndefined()
  })

  it('shows what is wrong and does not create', async () => {
    await open()
    button('Circular pocket').click()
    await nextTick()
    const diameter = input('Pocket diameter, mm')
    diameter.value = '5'
    diameter.dispatchEvent(new Event('input'))
    await nextTick()
    expect(wrapper?.text()).toContain('The tool (diameter 6) is too big for this pocket.')
    expect(button('Create and open').disabled).toBe(true)
    button('Create and open').click()
    expect(sent().map((command) => command.type)).toEqual(['list_directory'])
  })

  it('says that pockets plunge at the centre, and nothing for facing', async () => {
    await open()
    expect(document.body.textContent).not.toContain('cuts at its centre')
    button('Rectangular pocket').click()
    await nextTick()
    expect(document.body.textContent).toContain('use an end mill that cuts at its centre')
  })

  it('writes an inch program when the panel shows inches', async () => {
    panel.unit = 'inch'
    await open()
    button('Helical bore').click()
    await nextTick()
    expect(input('Bore diameter, in').getAttribute('data-numeric-unit')).toBe('in')
    button('Create and open').click()
    const save = sent().at(-1)
    if (save?.type !== 'save_program') throw new Error('No save')
    expect(save.path).toBe(`${programDirectory}/bore-0.375in.ngc`)
    expect(save.text).toContain('G17 G20 G40 G80 G90 G94\nT2 M6\nG43 H2\n')
  })
})

describe('save_program in the simulator', () => {
  it('writes a new program the browser can open, and never over another', () => {
    const r = rig()
    const path = `${programDirectory}/new.ngc`
    expect(r.send({ type: 'save_program', path: 'new.ngc', text: 'G0 X1\nM2\n' }).outcome).toBe('accepted')
    expect(r.send({ type: 'open_program', path }).outcome).toBe('accepted')
    expect(r.sim.status().program.file).toBe(path)
    expect(r.send({ type: 'save_program', path, text: 'M2\n' })).toMatchObject({ outcome: 'failed', reason: 'A program named new.ngc exists already.' })
    expect(r.send({ type: 'save_program', path, text: 'M2\n', replace: true })).toMatchObject({
      outcome: 'rejected', reason: 'The loaded program is not replaced. Choose another name.' })
    expect(r.send({ type: 'save_program', path: '/home/sim/elsewhere.ngc', text: 'M2\n' })).toMatchObject({
      outcome: 'failed', reason: 'New programs go in the program directory.' })
    expect(r.send({ type: 'save_program', path: '.hidden.ngc', text: 'M2\n' }).outcome).toBe('failed')
    expect(r.send({ type: 'save_program', path: 'empty.ngc', text: '' }).outcome).toBe('rejected')
    expect(r.send({ type: 'list_directory', path: null }).outcome).toBe('accepted')
    const listing = r.out.findLast((message) => message.type === 'directory')
    expect(listing?.type === 'directory' && listing.entries.some((entry) => entry.name === 'new.ngc')).toBe(true)
  })
})

describe('drilling cycles in the simulator', () => {
  const tools = [{ number: 1, pocket: 1, length: 0, diameter: 5, comment: '' }]
  function path(...lines: string[]): number[][] {
    const m = newModal(tools)
    const ops: Op[] = lines.flatMap((line, index) => interpretLine(m, line, index + 1, false).ops)
    return ops.flatMap((op) => op.kind === 'move' ? [[...(op.points.at(-1) ?? []).map((v) => Math.round(v * 1000) / 1000 + 0), op.motion === 'rapid' ? 0 : 1]] : [])
  }

  it('G81 drills each hole and G98 returns to the Z before the cycles', () => {
    expect(path('G0 Z10', 'G98 G81 X5 Y5 Z-3 R2 F100', 'X15')).toEqual([
      [0, 0, 10, 0], [5, 5, 10, 0], [5, 5, 2, 0], [5, 5, -3, 1], [5, 5, 10, 0],
      [15, 5, 10, 0], [15, 5, 2, 0], [15, 5, -3, 1], [15, 5, 10, 0],
    ])
  })

  it('G83 pecks back to R and G99 stays at R', () => {
    expect(path('G0 Z10', 'G99 G83 X0 Y0 Z-5 R1 Q3 F100')).toEqual([
      [0, 0, 10, 0], [0, 0, 10, 0], [0, 0, 1, 0], [0, 0, -2, 1], [0, 0, 1, 0], [0, 0, -1.746, 0], [0, 0, -5, 1], [0, 0, 1, 0],
    ])
  })

  it('refuses what LinuxCNC refuses', () => {
    const m = newModal(tools)
    expect(() => interpretLine(m, 'G81 X1 Y1 Z-1 F100', 1, false)).toThrow('G81 needs Z and R words')
    expect(() => interpretLine(m, 'G83 X1 Y1 Z-1 R1 F100', 1, false)).toThrow('G83 needs a positive Q word')
    expect(() => interpretLine(m, 'G80 X1', 1, false)).toThrow('G80: axis words without a motion code')
  })
})
