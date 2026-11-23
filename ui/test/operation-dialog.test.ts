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
function machineStatus(workZ = -100, g92Z = 0, maxZ = 0): void {
  const status = new SimMachine({ emit: (): void => undefined }).status()
  status.axes = status.axes.map((axis) => axis.axis === 'z' ? { ...axis, max_limit: maxZ } : axis)
  status.offsets.g5x[2] = workZ
  status.offsets.g92[2] = g92Z
  wire.receive(status)
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
  machineStatus()
  wire.receive({ type: 'tool_table', revision: 1, tools: [
    { number: 1, pocket: 1, length: 60, diameter: 50, comment: 'Face mill 50 mm' },
    { number: 2, pocket: 2, length: 40, diameter: 6, comment: 'End mill 6 mm' },
    { number: 3, pocket: 3, length: 70, diameter: 5, comment: 'Drill 5 mm' },
  ] })
  panel.unit = 'mm'
  panel.preferences.go_to_safe_z = null
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

  it('requires a thread mill selection and saves internal threads through the normal program picker', async () => {
    await open()
    button('Internal thread milling').click()
    await nextTick()
    const tool = wrapper?.get<HTMLSelectElement>('select[aria-label="Tool"]')
    expect(tool?.element.value).toBe('0')
    expect(button('Create and open').disabled).toBe(true)
    await tool?.setValue('2')
    button('Left-hand').click()
    await nextTick()
    button('Create and open').click()
    const save = sent().at(-1)
    expect(save).toMatchObject({ type: 'save_program', path: `${programDirectory}/thread-10mm-lh.ngc` })
    if (save?.type !== 'save_program') throw new Error('No thread program was saved')
    expect(save.text).toContain('T2 M6\nG43 H2')
    expect(save.text).toContain('G1 Z0.375 F50\nG3 X1.625 Y0 Z0 I0.8125 J0 F60')
    answer(save)
    await nextTick()
    expect(sent().at(-1)).toMatchObject({ type: 'open_program', path: save.path })
  })

  it('requires an explicit finishing tool and only displays dwell for boring', async () => {
    await open()
    button('Ream / bore').click()
    await nextTick()
    const tool = wrapper?.get<HTMLSelectElement>('select[aria-label="Tool"]')
    expect(tool?.element.value).toBe('0')
    expect(button('Create and open').disabled).toBe(true)
    expect(wrapper?.find('[aria-label="Bottom dwell, s"]').exists()).toBe(false)
    await tool?.setValue('2')
    button('Bore (G89)').click()
    await nextTick()
    expect(input('Bottom dwell, s').value).toBe('0.5')
    button('Create and open').click()
    const save = sent().at(-1)
    expect(save).toMatchObject({ type: 'save_program', path: `${programDirectory}/bore-grid-3x2.ngc` })
    if (save?.type !== 'save_program') throw new Error('No boring program was saved')
    expect(save.text).toContain('G98 G89 X10 Y10 Z-10 R2 P0.5 F60')
  })

  it('queues snapshots in order, discards a queued item and saves the job only', async () => {
    await open(['job-facing-100x60mm.ngc'])
    button('Add to job').click()
    await nextTick()
    const width = input('Size X, mm')
    width.value = '80'
    width.dispatchEvent(new Event('input'))
    await nextTick()
    button('Add to job').click()
    await nextTick()
    await wrapper?.get('.job-toggle').trigger('click')
    const queued = wrapper?.findAll('button[aria-label="Remove Facing from job"]') ?? []
    expect(queued).toHaveLength(2)
    await queued[1]?.trigger('click')

    button('Hole pattern').click()
    await nextTick()
    button('Add to job').click()
    await nextTick()
    button('Circular pocket').click()
    await nextTick()
    const diameter = input('Pocket diameter, mm')
    diameter.value = '5'
    diameter.dispatchEvent(new Event('input'))
    await nextTick()
    expect(button('Add to job').disabled).toBe(true)
    button('Review job').click()
    await nextTick()
    expect(sent().map((command) => command.type)).toEqual(['list_directory'])
    expect(wrapper?.findAll('.job li')).toHaveLength(2)
    expect(wrapper?.get('.job-height').text()).toBe('Tool changes at machine Z -0.5 mm')
    expect(button('Create job and open').disabled).toBe(false)
    button('Create job and open').click()
    const save = sent().at(-1)
    if (save?.type !== 'save_program') throw new Error('No job save')
    expect(save.path).toBe(`${programDirectory}/job-facing-100x60mm-2.ngc`)
    expect(save.text).toContain('Rectangle X 100 by Y 60')
    expect(save.text).not.toContain('Rectangle X 80')
    expect(save.text.indexOf('(Facing')).toBeLessThan(save.text.indexOf('(Hole pattern'))
    expect(save.text).not.toContain('(Circular pocket')
    expect(save.text).toContain('M5\n\nG90 G53 G0 Z-0.5\n\n(Hole pattern')
    expect(save.text.match(/^M2$/gm)).toEqual(['M2'])
    answer(save)
    expect(sent().at(-1)).toMatchObject({ type: 'open_program', path: save.path })
  })

  it('requires a machine Z above the G5x, G92 and G43 adjusted clearance', async () => {
    machineStatus(-100, 8)
    panel.preferences.go_to_safe_z = -17
    await open()
    button('Add to job').click()
    button('Hole pattern').click()
    await nextTick()
    button('Add to job').click()
    await nextTick()
    button('Review job').click()
    await nextTick()
    expect(wrapper?.get('[role="alert"]').text()).toContain('above every operation’s clearance')
    expect(button('Create job and open').disabled).toBe(true)
    expect(sent().map((command) => command.type)).toEqual(['list_directory'])
    panel.preferences.go_to_safe_z = -16.9
    await nextTick()
    expect(button('Create job and open').disabled).toBe(false)
    expect(wrapper?.get('.job-height').text()).toBe('Tool changes at machine Z -16.9 mm')
  })

  it('checks the emitted inch tool-change height against the machine soft limits', async () => {
    machineStatus(-100, 0, -2.541)
    panel.unit = 'inch'
    panel.preferences.go_to_safe_z = -2.541
    await open()
    button('Add to job').click()
    button('Hole pattern').click()
    await nextTick()
    button('Add to job').click()
    await nextTick()
    button('Review job').click()
    await nextTick()
    expect(wrapper?.get('[role="alert"]').text()).toContain('within the Z soft limits')
    expect(button('Create job and open').disabled).toBe(true)
    panel.preferences.go_to_safe_z = -2.543
    await nextTick()
    button('Create job and open').click()
    const save = sent().at(-1)
    if (save?.type !== 'save_program') throw new Error('No job save')
    expect(save.text).toContain('G90 G53 G0 Z-0.1001')
  })

  it('keeps the dialog with the reason when the save fails, and opens nothing', async () => {
    await open()
    button('Create and open').click()
    answer(sent().at(-1), 'failed', 'A program named facing-100x60mm.ngc exists already.')
    await nextTick()
    expect(wrapper?.text()).toContain('exists already')
    expect(sent().map((command) => command.type)).toEqual(['list_directory', 'save_program'])
    expect(wrapper?.emitted('close')).toBeUndefined()
    button('Circular pocket').click()
    await nextTick()
    const diameter = input('Pocket diameter, mm')
    diameter.value = '5'
    diameter.dispatchEvent(new Event('input'))
    await nextTick()
    expect(wrapper?.get('[role="alert"]').text()).toContain('too big for this pocket')
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

  it('lists the program directory once the link is live, and again after a failed listing', async () => {
    panel.link = 'connecting'
    wrapper = mount(OperationDialog, { attachTo: document.body, global: { stubs: { teleport: true } } })
    expect(sent()).toEqual([])
    panel.link = 'live'
    await nextTick()
    answer(sent().at(-1), 'failed', 'No answer from the machine')
    await nextTick()
    expect(wrapper.text()).toContain('No answer from the machine')
    panel.link = 'silent'
    await nextTick()
    panel.link = 'live'
    await nextTick()
    const again = sent().at(-1)
    expect(sent().map((command) => command.type)).toEqual(['list_directory', 'list_directory'])
    wire.receive({ type: 'directory', path: programDirectory, parent: null, entries: [] })
    answer(again)
    await nextTick()
    expect(wrapper.text()).not.toContain('No answer from the machine')
    expect(button('Create and open').disabled).toBe(false)
  })

  it('suggests the tool that is in the spindle now', async () => {
    await open()
    const status = new SimMachine({ emit: (): void => undefined }).status()
    expect(status.tool.in_spindle).not.toBe(2)
    wire.receive({ ...status, tool: { ...status.tool, in_spindle: 2 } })
    await nextTick()
    button('Create and open').click()
    const save = sent().at(-1)
    if (save?.type !== 'save_program') throw new Error('No save')
    expect(save.text).toContain('\nT2 M6\n')
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
