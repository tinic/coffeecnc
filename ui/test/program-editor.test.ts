// The program editor: its model, the component against a scripted machine, and the
// simulator's save over the loaded program.

import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { nextTick } from 'vue'

import ProgramEditor from '../src/components/ProgramEditor.vue'
import { setLanguage } from '../src/i18n'
import { EditHistory, findAll, lineOf, lineStart, maxProgramBytes, programSource, uneditableReason, type EditorTarget } from '../src/program-editor'
import type { FromMachine, ProgramText, Status, ToMachine } from '../src/protocol'
import { programDirectory, programStamp } from '../src/sim/files'
import { hello, SimMachine } from '../src/sim/machine'
import { rig, testProgram } from './harness'

const wire = vi.hoisted(() => ({ receive: (message: FromMachine): void => { throw new Error(`No listener for ${message.type}`) }, send: vi.fn<(command: ToMachine) => void>() }))
vi.mock('../src/machine', async () => {
  const { createPanel } = await import('../src/store')
  const instance = createPanel({ transport: { simulated: false, send: wire.send,
    listen: (listener): void => { wire.receive = listener } } })
  return { panel: instance.state, send: instance.send, onResult: instance.onResult }
})

const file = `${programDirectory}/part.ngc`
const lines = ['(Part)', 'G21 G90', 'G0 X10 Y5', 'G1 Z-1 F300', 'M2']
let wrapper: VueWrapper | undefined
let status: Status

function sent(): ToMachine[] {
  return wire.send.mock.calls.map(([command]) => command)
}
function answer(command: ToMachine | undefined, outcome: 'accepted' | 'failed' | 'rejected' = 'accepted', reason = ''): void {
  if (command === undefined) throw new Error('No command to answer')
  wire.receive({ type: 'result', id: command.id, outcome, reason })
}
// The editor is teleported into the workspace, as in the panel: a stubbed teleport would make
// its text area anew on every render.
function all(selector: string): HTMLElement[] {
  return [...document.querySelectorAll<HTMLElement>(`#workspace-dialogs ${selector}`)]
}
function one(selector: string): HTMLElement | null {
  return document.querySelector<HTMLElement>(`#workspace-dialogs ${selector}`)
}
function field(selector: string): HTMLInputElement {
  const found = one(selector)
  if (!(found instanceof HTMLInputElement)) throw new Error(`Missing field ${selector}`)
  return found
}
function text(selector = ''): string {
  return (selector === '' ? document.querySelector('#workspace-dialogs') : one(selector))?.textContent.trim() ?? ''
}
function button(label: string): HTMLButtonElement {
  const found = all('button').find((candidate) => candidate.textContent.trim() === label || candidate.getAttribute('aria-label') === label)
  if (!(found instanceof HTMLButtonElement)) throw new Error(`Missing button ${label}`)
  return found
}
function area(): HTMLTextAreaElement {
  const found = one('textarea[aria-label="Program text"]')
  if (!(found instanceof HTMLTextAreaElement)) throw new Error('Missing text area')
  return found
}
async function fill(selector: string, value: string): Promise<void> {
  const input = field(selector)
  input.value = value
  input.dispatchEvent(new Event('input'))
  await nextTick()
}
async function type(text: string): Promise<void> {
  const element = area()
  element.dispatchEvent(new InputEvent('beforeinput', { inputType: 'insertText', data: text, cancelable: true }))
  element.value = text
  element.dispatchEvent(new Event('input'))
  await nextTick()
}
function load(program: Partial<ProgramText> = {}): void {
  status = { ...status, program: { ...status.program, file, revision: 4 } }
  wire.receive({ type: 'program', file, revision: 4, lines, truncated: false, stamp: 'abc123', ...program })
  wire.receive(status)
}
async function open(target: EditorTarget = { kind: 'loaded' }): Promise<void> {
  wrapper = mount(ProgramEditor, { props: { target }, attachTo: document.body })
  await nextTick()
  await nextTick()
}

beforeEach(() => {
  document.body.innerHTML = '<div id="dialogs"></div><div id="workspace-dialogs"></div>'
  wire.send.mockReset()
  wire.receive({ ...hello, simulated: false })
  status = new SimMachine({ emit: (): void => undefined }).status()
  wire.receive(status)
})
afterEach(() => { wrapper?.unmount(); wrapper = undefined })

describe('program editor model', () => {
  it('holds the program with a final line end and finds what may not be edited', () => {
    expect(programSource(['G0 X1', 'M2'])).toBe('G0 X1\nM2\n')
    expect(programSource([])).toBe('')
    const program: ProgramText = { type: 'program', file, revision: 1, lines: ['M2'], stamp: '1' }
    expect(uneditableReason(program)).toBeNull()
    expect(uneditableReason({ ...program, truncated: true })).toContain('larger than the panel shows')
    expect(uneditableReason({ ...program, lines: ['(Fr�ser)'] })).toContain('not UTF-8')
    expect(uneditableReason({ ...program, lines: ['G'.repeat(maxProgramBytes)] })).toContain('512 KiB')
  })

  it('undoes typing as one step and every other edit on its own', () => {
    const history = new EditHistory()
    const at = (text: string): { text: string; start: number; end: number } => ({ text, start: text.length, end: text.length })
    history.record(at(''), 'insertText', 0, 1)
    history.record(at('G'), 'insertText', 100, 2)
    history.record(at('G0'), 'insertFromPaste', 200, 5)
    expect(history.undo(at('G0 X1'))).toEqual(at('G0'))
    expect(history.undo(at('G0'))).toEqual(at(''))
    expect(history.canUndo).toBe(false)
    expect(history.redo(at(''))).toEqual(at('G0'))
    history.record(at('G0'), 'insertText', 300, 3)
    expect(history.canRedo).toBe(false)
  })

  it('starts a new undo step when typing goes on elsewhere', () => {
    const history = new EditHistory()
    history.record({ text: 'G0\nM2\n', start: 2, end: 2 }, 'insertText', 0, 3)
    history.record({ text: 'G0 \nM2\n', start: 3, end: 3 }, 'insertText', 100, 4)
    // The caret moved to the start of the text, inside the merge time.
    history.record({ text: 'G0 X\nM2\n', start: 0, end: 0 }, 'insertText', 200, 1)
    expect(history.undo({ text: '(G0 X\nM2\n', start: 1, end: 1 })).toEqual({ text: 'G0 X\nM2\n', start: 0, end: 0 })
    expect(history.undo({ text: 'G0 X\nM2\n', start: 0, end: 0 })).toEqual({ text: 'G0\nM2\n', start: 2, end: 2 })
    expect(history.canUndo).toBe(false)
  })

  it('keeps no more history than it may', () => {
    const history = new EditHistory(3, 10)
    for (let n = 0; n < 6; n += 1) history.record({ text: 'G0 X1', start: 0, end: 0 }, 'insertFromPaste', n * 2000, 0)
    let steps = 0
    while (history.undo({ text: '', start: 0, end: 0 }) !== null) steps += 1
    expect(steps).toBe(2)
  })

  it('finds without regard to case and counts lines', () => {
    expect(findAll('G0 x1\nG1 X2', 'X')).toEqual([3, 9])
    expect(findAll('G0', '')).toEqual([])
    expect(lineOf('a\nb\nc', 0)).toBe(1)
    expect(lineOf('a\nb\nc', 4)).toBe(3)
    expect(lineStart('a\nbb\nc', 3)).toBe(5)
    expect(lineStart('a\nbb\nc', 9)).toBe(6)
  })
})

describe('program editor', () => {
  it('opens the loaded program with the code view colouring', async () => {
    load()
    await open()
    expect(area().value).toBe(`${lines.join('\n')}\n`)
    expect(area().readOnly).toBe(false)
    const tones = all('.gcode-tones [data-tone]').map((span) => [span.textContent, span.getAttribute('data-tone')])
    expect(tones).toContainEqual(['(Part)', 'comment'])
    expect(tones).toContainEqual(['G0', 'rapid'])
    expect(tones).toContainEqual(['X10', 'x'])
    expect(tones).toContainEqual(['F300', 'rate'])
    expect(all('.gutter .number').map((row) => row.textContent).slice(0, 3)).toEqual(['1', '2', '3'])
  })

  it('saves over the loaded program with its stamp; the machine opens it again', async () => {
    load()
    await open()
    expect(button('Save and reopen').disabled).toBe(true)
    await type('G21\nG0 X12\nM2\n')
    button('Save and reopen').click()
    await nextTick()
    const save = sent().at(-1)
    expect(save).toMatchObject({ type: 'save_program', path: file, text: 'G21\nG0 X12\nM2\n', stamp: 'abc123' })
    answer(save)
    await nextTick()
    expect(wrapper?.emitted('close')).toHaveLength(1)
    expect(sent().map((command) => command.type)).toEqual(['save_program'])
  })

  it('keeps the text and says why when the file changed on disk', async () => {
    load()
    await open()
    await type('M2\n')
    button('Save and reopen').click()
    answer(sent().at(-1), 'failed', 'The program file changed since it was opened. Open it again to edit it.')
    await nextTick()
    expect(text()).toContain('changed since it was opened')
    expect(area().value).toBe('M2\n')
    expect(area().readOnly).toBe(false)
    expect(wrapper?.emitted('close')).toBeUndefined()
  })

  it('saves a copy under a new name and opens it', async () => {
    load()
    await open()
    button('Save as…').click()
    await nextTick()
    expect(field('input[aria-label="File name"]').value).toBe('part-copy.ngc')
    await fill('input[aria-label="File name"]', 'part-b')
    button('Save and open').click()
    const save = sent().at(-1)
    expect(save).toEqual({ id: save?.id, type: 'save_program', path: `${programDirectory}/part-b.ngc`, text: `${lines.join('\n')}\n` })
    await nextTick()
    expect(area().readOnly).toBe(true)
    expect(button('Close').disabled).toBe(true)
    expect(button('Cancel').disabled).toBe(true)
    area().dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }))
    one('.scrim')?.dispatchEvent(new Event('pointerdown', { bubbles: true }))
    expect(wrapper?.emitted('close')).toBeUndefined()
    answer(save)
    await nextTick()
    const opened = sent().at(-1)
    expect(opened).toMatchObject({ type: 'open_program', path: `${programDirectory}/part-b.ngc` })
    answer(opened)
    await nextTick()
    expect(wrapper?.emitted('close')).toHaveLength(1)
  })

  it('writes a new program in the folder it was started from', async () => {
    await open({ kind: 'new', folder: programDirectory, taken: ['program.ngc'] })
    expect(text()).toContain('New program')
    expect(field('input[aria-label="File name"]').value).toBe('program-2.ngc')
    expect(button('Save and open').disabled).toBe(true)
    await type('G0 X0\nM2\n')
    button('Save and open').click()
    expect(sent().at(-1)).toMatchObject({ type: 'save_program', path: `${programDirectory}/program-2.ngc`, text: 'G0 X0\nM2\n' })
    expect(sent().at(-1)).not.toHaveProperty('stamp')
  })

  it('asks once before unsaved changes are dropped', async () => {
    load()
    await open()
    button('Cancel').click()
    await nextTick()
    expect(wrapper?.emitted('close')).toHaveLength(1)
    wrapper?.unmount()
    await open()
    await type('M2\n')
    button('Cancel').click()
    await nextTick()
    expect(text()).toContain('Discard changes?')
    expect(wrapper?.emitted('close')).toBeUndefined()
    button('Keep editing').click()
    await nextTick()
    expect(text()).not.toContain('Discard changes?')
    button('Close').click()
    await nextTick()
    button('Discard changes').click()
    expect(wrapper?.emitted('close')).toHaveLength(1)
  })

  it('starts a new program from inside the editor, asking once about unsaved changes', async () => {
    load()
    await open()
    button('New program').click()
    expect(wrapper?.emitted('newProgram')).toEqual([[programDirectory]])
    await type('M2\n')
    button('New program').click()
    await nextTick()
    expect(text()).toContain('Discard changes?')
    expect(wrapper?.emitted('newProgram')).toHaveLength(1)
    button('Discard changes').click()
    expect(wrapper?.emitted('newProgram')).toEqual([[programDirectory], [programDirectory]])
    expect(wrapper?.emitted('close')).toBeUndefined()
  })

  it('names a new program relative to the program directory when no folder is known', async () => {
    await open({ kind: 'new', folder: '', taken: [] })
    await type('M2\n')
    button('Save and open').click()
    expect(sent().at(-1)).toMatchObject({ type: 'save_program', path: 'program.ngc', text: 'M2\n' })
  })

  it('is read-only while a program runs or is paused, and says so in one line', async () => {
    load()
    await open()
    for (const interp of ['reading', 'paused'] as const) {
      wire.receive({ ...status, interp, mode: 'auto' })
      await nextTick()
      expect(area().readOnly).toBe(true)
      expect(text('.notice')).toBe('Read-only while a program runs or is paused. Stop it to edit.')
      expect(button('Save and reopen').disabled).toBe(true)
      expect(button('Save as…').disabled).toBe(true)
    }
    wire.receive(status)
    await nextTick()
    expect(area().readOnly).toBe(false)
    expect(one('.notice')).toBeNull()
  })

  it('does not edit a program the panel has only in part', async () => {
    load({ truncated: true })
    await open()
    expect(area().readOnly).toBe(true)
    expect(text()).toContain('larger than the panel shows')
  })

  it('waits for the program text, then shows it', async () => {
    await open()
    expect(text()).toContain('The program text has not arrived yet.')
    load()
    await nextTick()
    await nextTick()
    expect(area().value).toBe(`${lines.join('\n')}\n`)
    expect(text()).toContain('Edit part.ngc')
  })

  it('undoes and redoes with its buttons', async () => {
    load()
    await open()
    await type('M2\n')
    button('Undo').click()
    await nextTick()
    await nextTick()
    expect(area().value).toBe(`${lines.join('\n')}\n`)
    expect(button('Save and reopen').disabled).toBe(true)
    button('Redo').click()
    await nextTick()
    await nextTick()
    expect(area().value).toBe('M2\n')
  })

  it('undoes an input that did not announce itself, as a composition or execCommand', async () => {
    load()
    await open()
    area().value = 'G0 X1\n'
    area().dispatchEvent(new Event('input'))
    await nextTick()
    expect(button('Undo').disabled).toBe(false)
    button('Undo').click()
    await nextTick()
    expect(area().value).toBe(`${lines.join('\n')}\n`)
  })

  it('goes to Find with Ctrl+F in every language', async () => {
    await setLanguage('de')
    load()
    await open()
    const element = one('textarea')
    element?.focus()
    element?.dispatchEvent(new KeyboardEvent('keydown', { key: 'f', ctrlKey: true, bubbles: true, cancelable: true }))
    expect(document.activeElement).toBe(one('.find input'))
  })

  it('does not save an unmodified program with Ctrl+S', async () => {
    load()
    await open()
    area().dispatchEvent(new KeyboardEvent('keydown', { key: 's', ctrlKey: true, bubbles: true, cancelable: true }))
    await nextTick()
    expect(sent().filter((command) => command.type === 'save_program')).toEqual([])
  })

  it('does not save over a program without a stamp, and says to open it again', async () => {
    load({ stamp: '' })
    await open()
    await type('M2\n')
    expect(button('Save and reopen').disabled).toBe(true)
    expect(text('.block')).toBe('Open the program again before editing.')
    area().dispatchEvent(new KeyboardEvent('keydown', { key: 's', ctrlKey: true, bubbles: true, cancelable: true }))
    expect(sent().filter((command) => command.type === 'save_program')).toEqual([])
  })

  it('does not save while the machine moves, as the line edits', async () => {
    load()
    await open()
    await type('M2\n')
    for (const busy of [{ motion: { ...status.motion, in_position: false } }, { wait: { kind: 'tool_change' as const, tool: 2 } },
      { program: { ...status.program, breakpoint: { line: 2, reached: true } } }] as Partial<Status>[]) {
      wire.receive({ ...status, ...busy })
      await nextTick()
      expect(button('Save and reopen').disabled).toBe(true)
      expect(text('.block')).toBe('Stop the machine before saving.')
      area().dispatchEvent(new KeyboardEvent('keydown', { key: 's', ctrlKey: true, bubbles: true, cancelable: true }))
      expect(sent().filter((command) => command.type === 'save_program')).toEqual([])
    }
    wire.receive(status)
    await nextTick()
    expect(button('Save and reopen').disabled).toBe(false)
  })

  it('finds, marks and counts matches, and goes to a line', async () => {
    load()
    await open()
    await fill('input[aria-label="Find"]', 'g')
    expect(text('.find .count')).toBe('– of 4')
    expect(all('.marks .match').map((mark) => mark.textContent)).toEqual(['G', 'G', 'G', 'G'])
    button('Next match').click()
    await nextTick()
    expect(text('.find .count')).toBe('1 of 4')
    expect(text('.marks .current')).toBe('G')
    expect(area().selectionStart).toBe(7)
    await fill('input[aria-label="Go to line"]', '4')
    button('Go').click()
    expect(area().selectionStart).toBe(lineStart(`${lines.join('\n')}\n`, 4))
  })
})

describe('simulator save over the loaded program', () => {
  it('saves, opens the new text again and refuses a stale stamp or a running program', () => {
    const machine = rig()
    machine.home()
    machine.load('G21\nG1 X10 F300\nM2\n')
    const program = (): ProgramText => {
      const found = machine.out.findLast((message) => message.type === 'program')
      if (found?.type !== 'program') throw new Error('no program')
      return found
    }
    const first = program()
    // Where the cut ends in the preview: X of the last point of the last segment.
    const endX = (): number => {
      const points = machine.preview().segments.at(-1)?.points ?? []
      return points[points.length - 3] ?? Number.NaN
    }
    const before = endX()
    expect(first.stamp).toBe(programStamp('G21\nG1 X10 F300\nM2\n'))
    const saved = machine.send({ type: 'save_program', path: testProgram, text: 'G21\nG1 X20 F300\nM2\n', stamp: first.stamp })
    expect(saved.outcome).toBe('accepted')
    const second = program()
    expect(second.revision).toBeGreaterThan(first.revision)
    expect(second.lines).toEqual(['G21', 'G1 X20 F300', 'M2'])
    machine.sim.advance(400)
    expect(endX() - before).toBeCloseTo(10, 6)

    const stale = machine.send({ type: 'save_program', path: testProgram, text: 'M2\n', stamp: first.stamp })
    expect(stale).toMatchObject({ outcome: 'failed', reason: 'The program file changed since it was opened. Open it again to edit it.' })
    expect(program().lines).toEqual(['G21', 'G1 X20 F300', 'M2'])

    expect(machine.send({ type: 'run' }).outcome).toBe('accepted')
    const running = machine.send({ type: 'save_program', path: testProgram, text: 'M2\n', stamp: second.stamp })
    expect(running).toMatchObject({ outcome: 'rejected', reason: 'Stop the program before saving it.' })
  })
})
