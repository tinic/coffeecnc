import { afterEach, beforeEach, expect, it } from 'vitest'
import { effectScope, nextTick, reactive } from 'vue'

import { createProgramLineEdits } from '../src/program-line-edits'
import type { CommandBody, CommandResult, ProgramText, Status } from '../src/protocol'
import { SimMachine } from '../src/sim/machine'
import type { PanelState, ResultListener } from '../src/store'

const file = '/nc/part.ngc'
const original = ['G21', 'G0 X1', 'G1 X2 F200', 'M2']
let scope: ReturnType<typeof effectScope>
beforeEach(() => { scope = effectScope() })
afterEach(() => { scope.stop() })

interface TestContext {
  edits: ReturnType<typeof createProgramLineEdits>
  panel: Pick<PanelState, 'program' | 'status' | 'link'>
  sent: CommandBody[]
  load: (lines: string[], revision: number, path?: string) => void
  answer: (outcome?: CommandResult['outcome'], reason?: string) => void
  state: (interp: Status['interp']) => void
}

function setup(): TestContext {
  const status = new SimMachine({ emit: (): void => undefined }).status()
  const panel = reactive<Pick<PanelState, 'program' | 'status' | 'link'>>({ program: null, status, link: 'live' })
  const sent: CommandBody[] = []
  const listeners = new Set<ResultListener>()
  const edits = scope.run(() => createProgramLineEdits(panel, {
    send: (command): number => sent.push(command),
    onResult: (listener): (() => void) => {
      listeners.add(listener)
      return (): void => { listeners.delete(listener) }
    },
  }))
  if (edits === undefined) throw new Error('Line edits were not initialized')
  function load(lines: string[], revision: number, path = file): void {
    const program: ProgramText = { type: 'program', file: path, revision, lines, stamp: `stamp-${String(revision)}` }
    panel.status = { ...status, program: { ...status.program, file: path, revision } }
    panel.program = program
  }
  function answer(outcome: CommandResult['outcome'] = 'accepted', reason = ''): void {
    const command = sent.at(-1)
    if (command === undefined) throw new Error('Missing save request')
    for (const listener of [...listeners]) listener(command, { type: 'result', id: sent.length, outcome, reason })
  }
  function state(interp: Status['interp']): void {
    if (panel.status === null) throw new Error('Missing machine status')
    panel.status.interp = interp
  }
  load(original, 1)
  return { edits, panel, sent, load, answer, state }
}

it('cuts a selected range with the current stamp and only offers undo once the new text arrives', async () => {
  const { edits, sent, load, answer } = setup()
  edits.apply('cut', 2, 3)
  expect(sent).toEqual([{ type: 'save_program', path: file, stamp: 'stamp-1', text: 'G21\nM2\n' }])
  expect(edits.hasClipboard.value).toBe(false)
  answer()
  expect(edits.hasClipboard.value).toBe(true)
  expect(edits.canUndo.value).toBe(false)
  expect(edits.block.value).toContain('reopen')
  load(['G21', 'M2'], 2)
  await nextTick()
  expect(edits.canUndo.value).toBe(true)
  edits.undoLast()
  expect(sent.at(-1)).toMatchObject({ stamp: 'stamp-2', text: 'G21\nG0 X1\nG1 X2 F200\nM2\n' })
  load(original, 3)
  answer()
  await nextTick()
  expect(edits.canUndo.value).toBe(false)
  expect(edits.hasClipboard.value).toBe(true)
})

it('leaves copied lines intact when cut is refused and can insert on either side of a line', () => {
  const { edits, sent, answer, load } = setup()
  edits.apply('copy', 2, 2)
  expect(sent).toHaveLength(0)
  edits.apply('cut', 3, 3)
  answer('failed', 'The program changed on disk.')
  expect(edits.message.value).toMatchObject({ text: 'The program changed on disk.', error: true })
  edits.apply('insert-above', 1, 1)
  expect(sent.at(-1)).toMatchObject({ stamp: 'stamp-1', text: 'G0 X1\nG21\nG0 X1\nG1 X2 F200\nM2\n' })
  load(['G0 X1', ...original], 2)
  answer()
  edits.apply('insert-below', 5, 5)
  expect(sent.at(-1)).toMatchObject({ stamp: 'stamp-2', text: 'G0 X1\nG21\nG0 X1\nG1 X2 F200\nM2\nG0 X1\n' })
})

it('blocks edits during running, paused, partial, stale or empty programs', () => {
  const { edits, panel, sent, state } = setup()
  for (const running of ['reading', 'paused'] as const) {
    state(running)
    edits.apply('delete', 2, 2)
    expect(edits.block.value).toContain('Stop')
  }
  state('idle')
  if (!panel.program) throw new Error('Missing program')
  panel.program.truncated = true
  edits.apply('delete', 2, 2)
  panel.program.truncated = false
  panel.link = 'stale'
  edits.apply('delete', 2, 2)
  panel.link = 'live'
  edits.apply('delete', 1, 4)
  expect(edits.message.value?.text).toBe('The program would be empty.')
  expect(sent).toHaveLength(0)
})

it('does not undo over a new revision of the same file even when the text matches', async () => {
  const { edits, sent, answer, load } = setup()
  edits.apply('delete', 2, 2)
  const changed = ['G21', 'G1 X2 F200', 'M2']
  load(changed, 2)
  answer()
  expect(edits.canUndo.value).toBe(true)
  load(changed, 3)
  await nextTick()
  edits.undoLast()
  expect(edits.canUndo.value).toBe(false)
  expect(sent).toHaveLength(1)
})
