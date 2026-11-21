import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { shallowMount, type VueWrapper } from '@vue/test-utils'
import { nextTick } from 'vue'
import FileBrowser from '../src/components/FileBrowser.vue'
import ProgramTab from '../src/components/ProgramTab.vue'
import SetterStatusBand from '../src/components/SetterStatusBand.vue'
import NcText from '../src/components/NcText.vue'
import { panel } from '../src/machine'
import { SimMachine } from '../src/sim/machine'
import type { Tool } from '../src/protocol'
import type { PanelState } from '../src/store'

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

let wrapper: VueWrapper | null = null
beforeEach(() => {
  panel.link = 'live'
  panel.status = new SimMachine({ emit: (): void => undefined }).status()
  panel.status.machine = 'on'; panel.status.mode = 'auto'; panel.status.interp = 'reading'
  panel.status.program = { ...panel.status.program, file: '/parts/a.ngc', revision: 3, current_line: 200 }
  panel.program = { type: 'program', file: '/parts/a.ngc', revision: 3, lines: ['G0 X1', 'G0 Y2'], truncated: true }
  panel.directory = { type: 'directory', path: '/parts', parent: null, entries: [], truncated: true }
  panel.toolTable = null; panel.preview = null; panel.programLayout = 'text'; panel.unit = 'mm'
})
afterEach(() => { wrapper?.unmount() })

describe('partial machine snapshots', () => {
  it('labels partial text without treating the supplied line count as the full program length', async () => {
    wrapper = shallowMount(ProgramTab)
    expect(wrapper.text()).toContain('Partial program text')
    expect(wrapper.get('.status-row').text()).toContain('Line 200')
    expect(wrapper.get('.status-row').text()).not.toContain('/ 2')
    if (panel.program === null) throw new Error('Missing program')
    panel.program.truncated = false
    await nextTick()
    expect(wrapper.text()).not.toContain('Partial program text')
    expect(wrapper.get('.status-row').text()).not.toContain('/ 2')
    if (panel.status === null) throw new Error('Missing status')
    panel.status.program.current_line = 1
    await nextTick()
    expect(wrapper.get('.status-row').text()).toContain('Line 1 / 2')
    expect(wrapper.get('.status-row').text()).toContain('/ 2')
    panel.program.file = '/parts/different.ngc'
    await nextTick()
    expect(wrapper.text()).toContain('Waiting for program text')
    expect(wrapper.find('nc-text-stub').exists()).toBe(false)
  })

  it('does not describe an empty partial listing as an empty directory', async () => {
    wrapper = shallowMount(FileBrowser, { global: { stubs: { ModalDialog: { template: '<div><slot /></div>' } } } })
    expect(wrapper.text()).toContain('Partial directory listing')
    expect(wrapper.text()).not.toContain('This directory is empty.')
    if (panel.directory === null) throw new Error('Missing directory')
    panel.directory.truncated = false
    await nextTick()
    expect(wrapper.text()).not.toContain('Partial directory listing')
    expect(wrapper.text()).toContain('This directory is empty.')
  })
})

describe('program execution context', () => {
  it.each(['estop', 'off'] as const)('leaves %s to the top band and withholds stale pause and line data', (machine) => {
    if (panel.status === null || panel.program === null) throw new Error('Missing fixture')
    panel.status.machine = machine
    panel.status.interp = 'idle'; panel.status.wait = { kind: 'paused' }
    panel.status.program.current_line = 7; panel.status.program.motion_line = 7
    panel.program.lines = ['G21', 'G90', 'G0 X0', 'M2']; panel.program.truncated = false
    wrapper = shallowMount(ProgramTab)
    const footer = wrapper.get('.status-row').text()
    expect(footer).toBe('4 lines')
    expect(wrapper.getComponent(NcText).props()).toMatchObject({ currentLine: 0, motionLine: 0 })
  })

  it.each([1, 200])('withholds an idle counter of %i even if it fits in the current file', (line) => {
    if (panel.status === null || panel.program === null) throw new Error('Missing fixture')
    panel.status.interp = 'idle'; panel.status.wait = { kind: 'none' }
    panel.status.program.current_line = line; panel.status.program.motion_line = line
    panel.program.truncated = false
    wrapper = shallowMount(ProgramTab)
    expect(wrapper.get('.status-row').text()).toContain('Idle')
    expect(wrapper.get('.status-row').text()).toContain('2 lines')
    expect(wrapper.get('.status-row').text()).not.toContain('Line ')
    expect(wrapper.getComponent(NcText).props()).toMatchObject({ currentLine: 0, motionLine: 0 })
  })

  it('keeps AUTO pause highlights but does not attribute MDI counters to the loaded file', async () => {
    if (panel.status === null || panel.program === null) throw new Error('Missing fixture')
    panel.status.interp = 'paused'; panel.status.wait = { kind: 'paused' }
    panel.status.program.current_line = 2; panel.status.program.motion_line = 1
    panel.program.truncated = false
    wrapper = shallowMount(ProgramTab)
    expect(wrapper.get('.status-row').text()).not.toContain('Program paused')
    expect(wrapper.get('.status-row').text()).toContain('Line 2 / 2')
    expect(wrapper.getComponent(NcText).props()).toMatchObject({ currentLine: 2, motionLine: 1 })
    panel.status.mode = 'mdi'; panel.status.interp = 'reading'; panel.status.wait = { kind: 'none' }
    await nextTick()
    expect(wrapper.get('.status-row').text()).not.toContain('MDI command running')
    expect(wrapper.get('.status-row').text()).not.toContain('Line ')
    expect(wrapper.getComponent(NcText).props()).toMatchObject({ currentLine: 0, motionLine: 0 })
  })

  it.each(['stale', 'silent', 'down', 'connecting', 'incompatible'] satisfies PanelState['link'][])('withholds live program progress when the link is %s', async (link) => {
    if (panel.status === null) throw new Error('Missing fixture')
    panel.status.program.current_line = 2; panel.status.program.motion_line = 1
    wrapper = shallowMount(ProgramTab)
    panel.link = link
    await nextTick()
    expect(wrapper.get('.status-row').text()).not.toContain('Status unavailable')
    expect(wrapper.get('.status-row').text()).not.toContain('Line ')
    const text = wrapper.findComponent(NcText)
    if (text.exists()) expect(text.props()).toMatchObject({ currentLine: 0, motionLine: 0 })
  })

  it('withholds counters while program text belongs to an earlier revision', async () => {
    if (panel.status === null) throw new Error('Missing fixture')
    panel.status.program.current_line = 2; panel.status.program.motion_line = 1
    wrapper = shallowMount(ProgramTab)
    panel.status.program.revision += 1
    await nextTick()
    expect(wrapper.text()).toContain('Waiting for program text')
    expect(wrapper.findComponent(NcText).exists()).toBe(false)
    expect(wrapper.get('.status-row').text()).not.toContain('Line ')
  })

  it('reports a line outside the supplied text without clamping or highlighting the last row', () => {
    if (panel.status === null) throw new Error('Missing fixture')
    panel.status.program.motion_line = 150
    wrapper = shallowMount(ProgramTab)
    expect(wrapper.get('.status-row').text()).toContain('Line 200')
    expect(wrapper.get('.status-row').text()).not.toContain('/ 2')
    expect(wrapper.getComponent(NcText).props()).toMatchObject({ currentLine: 0, motionLine: 0 })
  })

  it('does not describe homing or manual motion as idle', async () => {
    if (panel.status === null) throw new Error('Missing fixture')
    panel.status.mode = 'manual'; panel.status.interp = 'idle'
    const axis = panel.status.axes[0]
    if (axis === undefined) throw new Error('Missing axis')
    axis.homing = true
    wrapper = shallowMount(ProgramTab)
    expect(wrapper.get('.status-row').text()).not.toMatch(/Homing in progress|idle/)
    expect(wrapper.getComponent(NcText).props()).toMatchObject({ currentLine: 0, motionLine: 0 })
    axis.homing = false; panel.status.motion.in_position = false
    await nextTick()
    expect(wrapper.get('.status-row').text()).toContain('Machine moving')
    expect(wrapper.get('.status-row').text()).not.toContain('idle')
  })
})

describe('status row', () => {
  // turo: the Tool in spindle block was redundant; the spindle and feed live in the status row.
  it('has no tool block, and shows the spindle and feed only while they run', async () => {
    if (panel.status === null) throw new Error('Missing status')
    panel.status.mode = 'manual'; panel.status.interp = 'idle'
    panel.status.spindle.direction = 'stopped'
    panel.status.motion.velocity = 0
    wrapper = shallowMount(ProgramTab)
    expect(wrapper.find('.tool-title').exists()).toBe(false)
    expect(wrapper.find('.status-row .spindle').exists()).toBe(false)
    expect(wrapper.find('.status-row .feed').exists()).toBe(false)
    panel.status.spindle.direction = 'forward'
    panel.status.spindle.commanded_rpm = 6000
    panel.status.motion.velocity = 600
    await nextTick()
    expect(wrapper.get('.status-row .spindle').text()).toContain('Spindle')
    expect(wrapper.get('.status-row .feed').text()).toBe('Feed 600 mm/min')
  })
})

describe('preview diagnostics', () => {
  it('shows why a matching preview is unavailable without inventing a source line', () => {
    if (panel.status === null) throw new Error('Missing fixture')
    panel.preview = { type: 'preview', context: panel.status.preview_context, state: 'unavailable',
      error: { line: 0, text: 'The preview helper is not installed.' }, segments: [], bounds: null, dwells: [] }
    wrapper = shallowMount(ProgramTab)
    expect(wrapper.find('.views > .preview-diagnostic').exists()).toBe(true)
    expect(wrapper.getComponent(SetterStatusBand).props('line')).toEqual({ text: 'The preview helper is not installed.', level: 'blocked' })
    expect(wrapper.find('.failure button').exists()).toBe(false)
    expect(wrapper.getComponent(NcText).props('errorLine')).toBe(0)
  })

  it.each([0, 200])('keeps a failure reason but offers no navigation to unavailable line %i', (line) => {
    if (panel.status === null) throw new Error('Missing fixture')
    panel.preview = { type: 'preview', context: panel.status.preview_context, state: 'failed',
      error: { line, text: 'Preview stopped before an unsupported operation.' }, segments: [], bounds: null, dwells: [] }
    wrapper = shallowMount(ProgramTab)
    expect(wrapper.get('.preview-diagnostic').text()).toContain('Preview stopped before an unsupported operation.')
    expect(wrapper.find('.failure button').exists()).toBe(false)
    expect(wrapper.getComponent(NcText).props('errorLine')).toBe(0)
  })

  it('offers navigation only while the failed line belongs to the displayed text', async () => {
    if (panel.status === null) throw new Error('Missing fixture')
    panel.preview = { type: 'preview', context: panel.status.preview_context, state: 'failed',
      error: { line: 2, text: 'Unsupported operation.' }, segments: [], bounds: null, dwells: [] }
    wrapper = shallowMount(ProgramTab)
    expect(wrapper.get('.failure button').text()).toBe('Show line 2')
    expect(wrapper.getComponent(NcText).props('errorLine')).toBe(2)
    panel.status.program.revision += 1
    await nextTick()
    expect(wrapper.get('.failure').text()).toContain('Unsupported operation.')
    expect(wrapper.find('.failure button').exists()).toBe(false)
    expect(wrapper.findComponent(NcText).exists()).toBe(false)
  })

  it.each(['failed', 'unavailable'] as const)('does not show an obsolete %s reason for the current context', (state) => {
    if (panel.status === null) throw new Error('Missing fixture')
    panel.preview = { type: 'preview', context: panel.status.preview_context - 1, state,
      error: { line: 2, text: 'An obsolete reason.' }, segments: [], bounds: null, dwells: [] }
    wrapper = shallowMount(ProgramTab)
    expect(wrapper.text()).not.toContain('An obsolete reason.')
    expect(wrapper.find('.failure button').exists()).toBe(false)
    expect(wrapper.getComponent(NcText).props('errorLine')).toBe(0)
  })
})
