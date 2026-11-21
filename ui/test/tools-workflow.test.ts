/* eslint-disable vue/one-component-per-file -- Separate mounted fixtures for tools and settings. */
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { defineComponent, nextTick } from 'vue'
import ToolsTab from '../src/components/ToolsTab.vue'
import SettingsDialog from '../src/components/SettingsDialog.vue'
import { panel, send } from '../src/machine'
import { defaultOperatorPreferences } from '../src/operator-settings'
import { createOperatorSettingsState } from '../src/operator-settings-state'
import { hello, SimMachine } from '../src/sim/machine'
import type { SetterState, Tool } from '../src/protocol'
import { setterState } from '../src/setter-state'
import type { ResultListener } from '../src/store'

const { resultListeners } = vi.hoisted(() => ({ resultListeners: new Set<ResultListener>() }))

vi.mock('../src/machine', async () => {
  const { computed, reactive } = await import('vue')
  const panel = reactive({ hello: null, toolModels: null, simulated: true }) as typeof import('../src/machine').panel
  const linkBlock = computed(() => panel.link !== 'live' || panel.status?.link !== 'live' ? 'Connection unavailable' : null)
  return { panel, linkBlock,
    homingActive: computed(() => panel.status?.axes.some((axis) => axis.homing) ?? false),
    allHomed: computed(() => panel.status?.axes.every((axis) => axis.homed) ?? false),
    mdiBlock: computed(() => linkBlock.value ?? (panel.status?.machine !== 'on' ? 'Machine is off'
      : panel.status.interp !== 'idle' ? 'Machine is busy' : null)),
    findTool: (number: number): Tool | null => panel.toolTable?.tools.find((tool) => tool.number === number) ?? null,
    send: vi.fn(), onResult: (listener: ResultListener): (() => void) => {
      resultListeners.add(listener)
      return () => { resultListeners.delete(listener) }
    } }
})
const fixture = defineComponent({ components: { ToolsTab },
  template: '<div><ToolsTab/><div id="dialogs"></div><div id="workspace-dialogs"></div></div>' })
let wrapper: VueWrapper | null = null
let dispose: (() => void) | null = null
function getWrapper(): VueWrapper { if (!wrapper) throw new Error('Missing wrapper'); return wrapper }
async function press(text: string): Promise<void> {
  const button = getWrapper().findAll('button').find((candidate) => candidate.text() === text)
  if (!button) throw new Error(`Missing ${text}`)
  await button.trigger('click')
}
beforeEach(() => {
  vi.stubGlobal('matchMedia', () => ({ matches: true }))
  setterState.value = null
  panel.hello = { ...hello }; panel.simulated = true; panel.link = 'live'; panel.unit = 'mm'
  panel.preferences = { ...defaultOperatorPreferences(), tool_reference_height: 6.35 }
  panel.status = new SimMachine({ emit: (): void => undefined }).status()
  panel.status.machine = 'on'; panel.status.link = 'live'; panel.status.tool.in_spindle = 1
  panel.toolTable = { type: 'tool_table', revision: 1, tools: [
    { number: 1, pocket: 1, diameter: 6, length: 25.4, comment: '6 mm end mill' },
    { number: 2, pocket: 2, diameter: 3, length: 12.7, comment: '3 mm long reach drill for the deep mounting holes' },
  ] }
  panel.workOffsets = null; vi.mocked(send).mockReset()
})
afterEach(() => {
  vi.unstubAllGlobals()
  const element = wrapper?.element as HTMLElement | undefined; wrapper?.unmount(); element?.remove(); wrapper = null
  dispose?.(); dispose = null
})
function mountTools(): void { wrapper = mount(fixture, { attachTo: document.body, global: { stubs: { GuideCanvas: true } } }) }

describe('fixed setter workflow', () => {
  it('previews a read-only length check and reports its result in the setter band', async () => {
    const status = panel.status
    if (!status) throw new Error('Missing status')
    panel.hello = { ...hello, fixed_setter: {
      reference_tool: 98, travel_mm: 15, feed_mm_min: 240, xy_tolerance_mm: 0.5, check_tolerance_mm: 0.1,
    } }
    status.spindle.actual_rpm = 0
    status.axes.forEach(axis => { axis.machine = 0 })
    status.motion.in_position = true
    mountTools()
    const answer = async (setter: SetterState): Promise<void> => {
      const command = vi.mocked(send).mock.lastCall?.[0]
      if (!command) throw new Error('Missing request')
      for (const listener of resultListeners)
        listener(command, { type: 'result', id: 1, outcome: 'accepted', reason: '', setter })
      await nextTick()
    }
    const state: SetterState = { calibration: [0, 0, -40], measurement: null }
    await answer(state)
    await press('Check length')
    expect(getWrapper().get('[role="dialog"]').attributes('aria-label')).toBe('Check tool length')
    expect(getWrapper().get('[role="dialog"]').text()).toContain('±0.1 mm')
    expect(send).toHaveBeenCalledTimes(1)
    await press('Start check')
    expect(send).toHaveBeenLastCalledWith({ type: 'fixed_setter', action: 'check', tool: 1, x: 0, y: 0, z: 0 })
    expect(getWrapper().get('.setter-band p').text()).toBe('Checking length…')
    await answer({ ...state, measurement: { calibration: false, tool: 1, contact: [0, 0, -13],
      value: 25.34, previous_length: 25.4, check_tolerance_mm: 0.1 } })
    expect(getWrapper().get('.setter-band p').text()).toBe('T01 passed · 0.060 mm from table')
    expect(getWrapper().find('.touched').exists()).toBe(false)

    await press('Check length')
    await press('Start check')
    status.interp = 'reading'
    await nextTick()
    const command = vi.mocked(send).mock.lastCall?.[0]
    if (!command) throw new Error('Missing request')
    const reason = 'T1 check failed. Tool is 0.2500 mm shorter.'
    status.interp = 'idle'
    for (const listener of resultListeners)
      listener(command, { type: 'result', id: 2, outcome: 'failed', reason })
    await nextTick()
    expect(getWrapper().get('.setter-band p').text()).toBe(reason)
  })

  it('explains a blocked setter press in place while the machine is off', async () => {
    if (!panel.status) throw new Error('Missing status')
    panel.status.machine = 'off'
    panel.hello = { ...hello, fixed_setter: { reference_tool: 98, travel_mm: 15, feed_mm_min: 240, xy_tolerance_mm: 0.5 } }
    mountTools()
    const command = vi.mocked(send).mock.lastCall?.[0]
    if (!command) throw new Error('Missing request')
    for (const listener of resultListeners)
      listener(command, { type: 'result', id: 1, outcome: 'accepted', reason: '' })
    await nextTick()
    expect(getWrapper().get('.setter-band').text()).toBe('')
    expect(getWrapper().get('.setter-action button').attributes('disabled')).toBeUndefined()
    vi.mocked(send).mockClear()
    await press('Touch off')
    await press('Start probe')
    expect(getWrapper().get('.setter-band p').text()).toBe('Machine is off')
    expect(send).not.toHaveBeenCalled()
  })
  it('previews teaching and measuring, and sends the same command only on Start', async () => {
    const status = panel.status
    if (!status) throw new Error('Missing status')
    panel.hello = { ...hello, fixed_setter: { reference_tool: 98, travel_mm: 15, feed_mm_min: 240, xy_tolerance_mm: 0.5 } }
    status.spindle.actual_rpm = 0
    status.probe_input = false
    status.axes.forEach((axis) => { axis.homed = true; axis.machine = 0 })
    status.motion.in_position = true
    let nextId = 0
    vi.mocked(send).mockImplementation(() => ++nextId)
    const answer = async (setter: SetterState): Promise<void> => {
      const command = vi.mocked(send).mock.lastCall?.[0]
      if (!command) throw new Error('Missing command')
      for (const listener of resultListeners) {
        listener(command, { type: 'result', id: nextId, outcome: 'accepted', reason: '', setter })
      }
      await nextTick()
    }
    status.tool.in_spindle = 98
    const state: SetterState = { calibration: [0, 0, -40], measurement: null }
    mountTools()
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'fixed_setter', action: 'status' })
    await answer({ calibration: null, measurement: null })
    await press('Touch off')
    expect(getWrapper().get('[role="dialog"]').attributes('aria-label')).toBe('Teach the tool setter')
    await press('Cancel')
    expect(send).toHaveBeenCalledTimes(1)
    await press('Touch off')
    await press('Start probe')
    expect(send).toHaveBeenLastCalledWith({ type: 'fixed_setter', action: 'touch_off', tool: 98, x: 0, y: 0, z: 0 })
    await answer({ ...state, measurement: { calibration: true, tool: 98, contact: [0, 0, -13], value: -40, previous_length: 27, stopping_distance: 0.03 } })
    expect(getWrapper().get('.setter-band p').text()).toBe('Setter taught')
    status.tool.in_spindle = 1
    await nextTick()
    await press('Touch off')
    expect(getWrapper().get('[role="dialog"]').attributes('aria-label')).toBe('Measure tool length')
    await press('Cancel')
    expect(send).toHaveBeenCalledTimes(2)
    await press('Touch off')
    await press('Start probe')
    expect(send).toHaveBeenLastCalledWith({ type: 'fixed_setter', action: 'touch_off', tool: 1, x: 0, y: 0, z: 0 })
    await answer({ ...state, measurement: { calibration: false, tool: 1, contact: [0, 0, -13], value: 27, previous_length: 25.4, stopping_distance: 0.03 }, measured_tools: [98, 1] })
    expect(getWrapper().get('.setter-band p').text()).toBe('T01 measured')
    expect(getWrapper().findAll('.tool-row').filter((row) => row.find('.touched').exists()).map((row) => row.find('.tool-number').text())).toContain('T01')
    expect(send).toHaveBeenCalledTimes(3)
    expect(getWrapper().find('[role="dialog"]').exists()).toBe(false)
  })

  it('after a restart shows saved marks and lets a touched-off tool teach the setter', async () => {
    const status = panel.status
    if (!status) throw new Error('Missing status')
    panel.hello = { ...hello, fixed_setter: { reference_tool: 98, travel_mm: 15, feed_mm_min: 240, xy_tolerance_mm: 0.5 } }
    status.spindle.actual_rpm = 0
    status.axes.forEach((axis) => { axis.machine = 0 })
    status.motion.in_position = true
    status.tool.in_spindle = 1
    let nextId = 0
    vi.mocked(send).mockImplementation(() => ++nextId)
    const answer = async (setter: SetterState): Promise<void> => {
      const command = vi.mocked(send).mock.lastCall?.[0]
      if (!command) throw new Error('Missing command')
      for (const listener of resultListeners)
        listener(command, { type: 'result', id: nextId, outcome: 'accepted', reason: '', setter })
      await nextTick()
    }
    mountTools()
    // Restarted: no taught setter, but the saved marks still match the tool table.
    await answer({ calibration: null, measurement: null, measured_tools: [1, 98] })
    expect(getWrapper().findAll('.tool-row').filter((row) => row.find('.touched').exists()).map((row) => row.find('.tool-number').text())).toContain('T01')
    await press('Touch off')
    expect(getWrapper().get('[role=dialog]').attributes('aria-label')).toBe('Teach the tool setter')
    await press('Start probe')
    expect(send).toHaveBeenLastCalledWith({ type: 'fixed_setter', action: 'touch_off', tool: 1, x: 0, y: 0, z: 0 })
    await answer({ calibration: [0, 0, -40], measurement: { calibration: true, tool: 1, contact: [0, 0, -13], value: -40, previous_length: 25.4, stopping_distance: 0.03 }, measured_tools: [1, 98] })
    expect(getWrapper().get('.setter-band p').text()).toBe('Setter taught')
  })

  it('asks for the reference or a touched-off tool when an unmarked tool has no taught setter', async () => {
    const status = panel.status
    if (!status) throw new Error('Missing status')
    panel.hello = { ...hello, fixed_setter: { reference_tool: 98, travel_mm: 15, feed_mm_min: 240, xy_tolerance_mm: 0.5 } }
    status.spindle.actual_rpm = 0
    status.axes.forEach((axis) => { axis.machine = 0 })
    status.motion.in_position = true
    status.tool.in_spindle = 2
    vi.mocked(send).mockImplementation(() => 1)
    mountTools()
    const command = vi.mocked(send).mock.lastCall?.[0]
    if (!command) throw new Error('Missing command')
    for (const listener of resultListeners)
      listener(command, { type: 'result', id: 1, outcome: 'accepted', reason: '', setter: { calibration: null, measurement: null, measured_tools: [1, 98] } })
    await nextTick()
    expect(getWrapper().get('.setter-band p').text()).toBe('Teach the setter with T98 or a touched-off tool first')
  })
})

describe('tool table workflow', () => {
  it('sorts a new tool snapshot by tool number without rearranging the source', async () => {
    const table = panel.toolTable
    if (table === null) throw new Error('Missing tool table')
    const highTool = { number: 99, pocket: 1, diameter: 3, length: 20, comment: 'Touch probe' }
    const reference = { number: 98, pocket: 99, diameter: 6.35, length: 30, comment: 'Reference tool' }
    panel.toolTable = { ...table, tools: [...table.tools, highTool] }
    mountTools()

    panel.toolTable = { ...table, revision: 2, tools: [...table.tools, highTool, reference] }
    await nextTick()

    const numberCells = getWrapper().findAll('.tool-row td:first-child')
    expect(numberCells.map((cell) => cell.text())).toEqual(['T01', 'T02', 'T98', 'T99'])
    expect(panel.toolTable.tools.map((tool) => tool.number)).toEqual([1, 2, 99, 98])
  })
  it('requires a separate confirmation before loading and applying a tool length', async () => {
    mountTools()
    await getWrapper().get('[aria-label="Load tool 2"]').trigger('click')
    expect(getWrapper().get('[role="dialog"]').attributes('aria-label')).toBe('Load T2?')
    expect(getWrapper().get('[role="dialog"]').text()).toContain('12.700')
    expect(send).not.toHaveBeenCalled()
    await press('Load T2')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'mdi', text: 'T2 M6 G43 H2' })
    expect(getWrapper().find('[role="dialog"]').exists()).toBe(false)
  })
  it.each(['off', 'busy', 'stale', 'reconnect', 'tool-change'] as const)('invalidates a pending load after %s', async (change) => {
    mountTools(); await getWrapper().get('[aria-label="Load tool 2"]').trigger('click')
    if (!panel.status) throw new Error('Missing status')
    if (change === 'off') panel.status.machine = 'off'
    if (change === 'busy') panel.status.interp = 'reading'
    if (change === 'stale') panel.link = 'stale'
    if (change === 'reconnect') panel.hello = { ...hello }
    if (change === 'tool-change') panel.status.tool.in_spindle = 3
    await nextTick()
    expect(getWrapper().find('[role="dialog"]').exists()).toBe(false)
    expect(send).not.toHaveBeenCalled()
  })
  it('blocks a load if the row changes after confirmation opens', async () => {
    mountTools(); await getWrapper().get('[aria-label="Load tool 2"]').trigger('click')
    const tool = panel.toolTable?.tools[1]; if (!tool) throw new Error('Missing tool')
    tool.length = 50; await nextTick()
    await press('Load T2'); expect(send).not.toHaveBeenCalled()
    expect(getWrapper().text()).toContain('The tool table changed')
  })
  it('selects a tapped row and changes selection without opening a dialog or sending a command', async () => {
    mountTools()
    const [first, second] = getWrapper().findAll('.tool-row')
    if (!first || !second) throw new Error('Missing tool rows')
    await second.get('.comment').trigger('click')
    expect(second.classes()).toContain('selected')
    expect(second.get('[aria-label="Select tool 2"]').attributes('aria-pressed')).toBe('true')
    expect(first.classes()).toContain('loaded')
    await first.get('[aria-label="Select tool 1"]').trigger('click')
    expect(first.classes()).toContain('selected')
    expect(second.classes()).not.toContain('selected')
    expect(getWrapper().find('[role="dialog"]').exists()).toBe(false)
    expect(send).not.toHaveBeenCalled()
  })
  it('opens editing from the Edit button, keeps model controls inside, and confirms deletion there', async () => {
    mountTools()
    expect(getWrapper().text()).not.toContain('Model defaults')
    expect(getWrapper().findAll('.tool-row .actions button').map((button) => button.text())).toEqual(['Edit', 'Load', 'Edit', 'Load'])
    await getWrapper().get('[aria-label="Edit tool 2"]').trigger('click')
    expect(getWrapper().get('[role="dialog"]').attributes('aria-label')).toBe('Edit tool 2')
    expect(getWrapper().get('[role="dialog"]').text()).toContain('Preview shape')
    expect(getWrapper().get('[role="dialog"]').text()).not.toContain('Model defaults')
    await press('Delete'); expect(send).not.toHaveBeenCalled()
    expect(getWrapper().get('[role="dialog"]').attributes('aria-label')).toBe('Remove tool 2?')
    await press('Remove tool'); expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'remove_tool', number: 2 })
  })
  it('cannot delete the spindle tool', async () => {
    mountTools(); await getWrapper().get('[aria-label="Edit tool 1"]').trigger('click')
    const deletion = getWrapper().findAll('button').find((button) => button.text() === 'Delete')
    expect(deletion?.attributes('disabled')).toBeDefined()
  })
  it('keeps unchanged length and diameter exact when editing a description in inches', async () => {
    panel.unit = 'inch'
    const tool = panel.toolTable?.tools[0]; if (!tool) throw new Error('Missing tool')
    tool.length = 1.234567891; tool.diameter = 6.123456789
    mountTools(); await getWrapper().get('[aria-label="Edit tool 1"]').trigger('click')
    await getWrapper().get('.description-entry input').setValue('Updated description')
    await press('Save tool')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'set_tool', tool: { ...tool, comment: 'Updated description' } })
  })
  it.each(['mm', 'inch'] as const)('prefills reference height in %s and retains the draft during position updates', async (unit) => {
    panel.unit = unit; mountTools(); await press('Touch off length')
    const field = getWrapper().get('[role="dialog"] input')
    expect((field.element as HTMLInputElement).value).toBe(unit === 'mm' ? '6.35' : '0.25')
    if (!panel.status) throw new Error('Missing status')
    panel.status = { ...panel.status, axes: panel.status.axes.map((axis) => ({ ...axis, work: axis.work + .001 })) }
    await nextTick(); expect(getWrapper().get('[role="dialog"] input').element).toBe(field.element)
    await press('Set T1 length')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'tool_touch_off', value: 6.35 })
  })
  it('preserves an unedited precise reference and accepts mixed-unit touch-off formulas', async () => {
    panel.preferences.tool_reference_height = 1.234567891; mountTools()
    await press('Touch off length'); await press('Set T1 length')
    expect(send).toHaveBeenLastCalledWith({ type: 'tool_touch_off', value: 1.234567891 })
    await press('Touch off length'); await getWrapper().get('[role="dialog"] input').setValue('7/2mm')
    await press('Set T1 length'); expect(send).toHaveBeenLastCalledWith({ type: 'tool_touch_off', value: 3.5 })
  })
  it('invalidates touch-off when the loaded tool or coordinate system changes', async () => {
    mountTools(); await press('Touch off length')
    if (!panel.status) throw new Error('Missing status')
    panel.status.tool.in_spindle = 2; await nextTick()
    expect(getWrapper().find('[role="dialog"]').exists()).toBe(false)
    await press('Touch off length'); panel.status.offsets.wcs = 2; await nextTick()
    expect(getWrapper().find('[role="dialog"]').exists()).toBe(false)
    expect(send).not.toHaveBeenCalled()
  })
  it('saves a reference-height formula as millimeters without issuing a machine command', async () => {
    const storage = { getItem: (): null => null, setItem: vi.fn() }
    const settings = createOperatorSettingsState(panel, { send, onResult: (): (() => void) => () => undefined }, () => storage)
    dispose = settings.dispose
    wrapper = mount(defineComponent({ components: { SettingsDialog }, setup: () => ({ settings }),
      template: '<div><SettingsDialog :settings="settings"/><div id="workspace-dialogs"></div></div>' }), { attachTo: document.body })
    await nextTick(); await getWrapper().get('#settings-reference').setValue('1/4in')
    await press('Save settings')
    expect(panel.preferences.tool_reference_height).toBe(6.35)
    expect(JSON.parse(storage.setItem.mock.calls[0]?.[1] as string)).toMatchObject({ tool_reference_height: 6.35 })
    expect(send).not.toHaveBeenCalled()
  })
})

// Operator scenarios of the tool editor: Save is always offered, and a press saves or says why not.
describe('tool editor scenarios', () => {
  const saveButton = (): ReturnType<VueWrapper['get']> => {
    const button = getWrapper().findAll('button').find((candidate) => candidate.text() === 'Save tool')
    if (!button) throw new Error('Missing Save tool')
    return button
  }
  it('writes the edited row over one changed elsewhere since the editor opened', async () => {
    mountTools(); await getWrapper().get('[aria-label="Edit tool 1"]').trigger('click')
    const tool = panel.toolTable?.tools[0]; if (!tool) throw new Error('Missing tool')
    const before = { ...tool }
    tool.length = 100; await nextTick()
    await getWrapper().get('.description-entry input').setValue('Re-ground')
    expect(saveButton().attributes('disabled')).toBeUndefined()
    await press('Save tool')
    expect(send).toHaveBeenCalledWith({ type: 'set_tool', tool: { ...before, comment: 'Re-ground' } })
  })
  it('keeps Save enabled with an invalid entry and says what to fix on press', async () => {
    mountTools(); await getWrapper().get('[aria-label="Edit tool 1"]').trigger('click')
    const length = getWrapper().findAll('[role="dialog"] input').find((input) => input.element.closest('label')?.textContent.startsWith('Length'))
    if (!length) throw new Error('Missing length field')
    await length.setValue('abc')
    expect(saveButton().attributes('disabled')).toBeUndefined()
    await press('Save tool')
    expect(send).not.toHaveBeenCalled()
    expect(getWrapper().get('[role="dialog"] .feedback').text()).toBe('Check the tool number, length and diameter.')
  })
  it('keeps Save enabled while a program runs and says why on press', async () => {
    mountTools(); await getWrapper().get('[aria-label="Edit tool 1"]').trigger('click')
    if (!panel.status) throw new Error('Missing status')
    panel.status = { ...panel.status, interp: 'reading' }; await nextTick()
    expect(saveButton().attributes('disabled')).toBeUndefined()
    await press('Save tool')
    expect(send).not.toHaveBeenCalled()
    expect(getWrapper().get('[role="dialog"] .feedback').text()).toBe('Stop execution before editing the tool.')
  })
  it('opens the machine touch probe tool as a touch probe and saves it', async () => {
    panel.hello = { ...hello, touch_probe_tool: 99 }
    const table = panel.toolTable; if (!table) throw new Error('Missing tool table')
    panel.toolTable = { ...table, tools: [...table.tools, { number: 99, pocket: 99, diameter: 2, length: 50, comment: '' }] }
    mountTools()
    expect(getWrapper().findAll('.tool-row').at(-1)?.text()).toContain('Touch probe')
    await getWrapper().get('[aria-label="Edit tool 99"]').trigger('click')
    expect((getWrapper().get('[aria-label="Tool type"]').element as HTMLSelectElement).value).toBe('touch-probe')
    expect(getWrapper().get('[role="dialog"]').text()).toContain('Ball diameter')
    await getWrapper().get('.description-entry input').setValue('Ruby probe')
    expect(saveButton().attributes('disabled')).toBeUndefined()
    await press('Save tool')
    expect(send).toHaveBeenCalledWith({ type: 'set_tool', tool: { number: 99, pocket: 99, diameter: 2, length: 50, comment: 'Ruby probe' } })
  })
})

describe('tool preview scenarios', () => {
  it('opens a read-only preview on a double tap of a tool row, not the editor', async () => {
    mountTools()
    const row = getWrapper().findAll('.tool-row')[1]; if (!row) throw new Error('Missing row')
    await row.trigger('click'); await row.trigger('click')
    const dialog = getWrapper().get('[role="dialog"]')
    expect(dialog.attributes('aria-label')).toBe('T02 3 mm long reach drill for the deep mounting holes')
    expect(dialog.find('input').exists()).toBe(false)
    expect(dialog.text()).toContain('12.700 mm')
    await press('Close')
    expect(getWrapper().find('[role="dialog"]').exists()).toBe(false)
    expect(send).not.toHaveBeenCalled()
  })
  it('selects on a single tap and does not open a preview for taps on different rows', async () => {
    mountTools()
    const rows = getWrapper().findAll('.tool-row')
    await rows[0]?.trigger('click'); await rows[1]?.trigger('click')
    expect(getWrapper().find('[role="dialog"]').exists()).toBe(false)
    expect(rows[1]?.classes()).toContain('selected')
  })
  it('shows the machine touch probe with its ball diameter', async () => {
    panel.hello = { ...hello, touch_probe_tool: 99 }
    const table = panel.toolTable; if (!table) throw new Error('Missing tool table')
    panel.toolTable = { ...table, tools: [...table.tools, { number: 99, pocket: 99, diameter: 2, length: 53.185, comment: '' }] }
    mountTools()
    const row = getWrapper().findAll('.tool-row').at(-1); if (!row) throw new Error('Missing row')
    await row.trigger('click'); await row.trigger('click')
    const dialog = getWrapper().get('[role="dialog"]')
    expect(dialog.attributes('aria-label')).toBe('T99 Touch probe')
    expect(dialog.text()).toContain('Ball diameter')
  })
})
