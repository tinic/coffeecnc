// The time left of a running program, with a subroutine that repeats its line numbers.

import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { shallowMount, type VueWrapper } from '@vue/test-utils'
import { nextTick } from 'vue'
import ProgramTab from '../src/components/ProgramTab.vue'
import { panel } from '../src/machine'
import { runClock } from '../src/run-clock'
import { hello, SimMachine } from '../src/sim/machine'
import type { PreviewSegment, Tool } from '../src/protocol'

vi.mock('../src/machine', async () => {
  const { computed, reactive } = await import('vue')
  const { defaultOperatorPreferences } = await import('../src/operator-settings')
  const { requiresHoming } = await import('../src/machine-capabilities')
  const panel = reactive({ hello: null, toolModels: null, simulated: true, preferences: defaultOperatorPreferences() }) as typeof import('../src/machine').panel
  return { panel, send: vi.fn(), findTool: (number: number): Tool | null => panel.toolTable?.tools.find((tool) => tool.number === number) ?? null,
    onResult: (): (() => void) => () => undefined,
    homingRequired: computed(() => requiresHoming(panel.hello)),
    allHomed: computed(() => panel.status?.axes.every((axis) => axis.homed) ?? false),
    live: computed(() => panel.link === 'live'), reported: computed(() => panel.link === 'down' ? null : panel.status) }
})

// 10 mm at F600: 1 s each. Lines 20 and 21 are a subroutine called from lines 3 and 7.
const move = (line: number): PreviewSegment => ({ line, kind: 'feed', tool: 1, feed: 600, points: [0, 0, 0, 10, 0, 0] })
const lines = [2, 20, 21, 6, 20, 21, 8]

let wrapper: VueWrapper | null = null
beforeEach(() => {
  panel.link = 'live'
  panel.hello = hello
  panel.status = new SimMachine({ emit: (): void => undefined }).status()
  panel.status.machine = 'on'; panel.status.mode = 'auto'; panel.status.interp = 'reading'
  panel.status.program = { ...panel.status.program, file: '/parts/a.ngc', revision: 3, current_line: 2, motion_line: 2 }
  panel.program = { type: 'program', file: '/parts/a.ngc', revision: 3, lines: Array.from({ length: 22 }, () => 'G1 X1'), truncated: false }
  panel.preview = { type: 'preview', context: panel.status.preview_context, state: 'ready', error: null,
    segments: lines.map(move), bounds: null, dwells: [] }
  panel.toolTable = null; panel.programLayout = 'text'; panel.unit = 'mm'
  runClock.startedAt = 0; runClock.now = 10_000
})
afterEach(() => { wrapper?.unmount(); runClock.startedAt = null })

async function at(line: number): Promise<string> {
  if (panel.status === null) throw new Error('Missing status')
  panel.status.program.motion_line = line
  panel.status.program.current_line = line
  await nextTick()
  return wrapper?.get('.run-time').text() ?? ''
}

describe('time left', () => {
  it('follows the run into the second call of a subroutine', async () => {
    wrapper = shallowMount(ProgramTab)
    expect(await at(2)).toBe('0:10 elapsed · 0:07 left')
    expect(await at(20)).toBe('0:10 elapsed · 0:06 left')
    expect(await at(6)).toBe('0:10 elapsed · 0:04 left')
    expect(await at(20)).toBe('0:10 elapsed · 0:03 left')
    expect(await at(8)).toBe('0:10 elapsed · 0:01 left')
  })
})
