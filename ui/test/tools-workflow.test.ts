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
import type { Tool } from '../src/protocol'

vi.mock('../src/machine', async () => {
  const { computed, reactive } = await import('vue')
  const panel = reactive({ hello: null, toolModels: null, simulated: true }) as typeof import('../src/machine').panel
  const linkBlock = computed(() => panel.link !== 'live' || panel.status?.link !== 'live' ? 'Connection unavailable' : null)
  return { panel, linkBlock,
    mdiBlock: computed(() => linkBlock.value ?? (panel.status?.machine !== 'on' ? 'Machine is off'
      : panel.status.interp !== 'idle' ? 'Machine is busy' : null)),
    findTool: (number: number): Tool | null => panel.toolTable?.tools.find((tool) => tool.number === number) ?? null,
    send: vi.fn(), onResult: (): (() => void) => () => undefined }
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
  panel.hello = { ...hello }; panel.simulated = true; panel.link = 'live'; panel.unit = 'mm'
  panel.preferences = { ...defaultOperatorPreferences(), tool_reference_height: 6.35 }
  panel.status = new SimMachine({ emit: (): void => undefined }).status()
  panel.status.machine = 'on'; panel.status.link = 'live'; panel.status.tool.in_spindle = 1
  panel.toolTable = { type: 'tool_table', revision: 1, tools: [
    { number: 1, pocket: 1, diameter: 6, length: 25.4, comment: '6 mm end mill' },
    { number: 2, pocket: 2, diameter: 3, length: 12.7, comment: '3 mm long reach drill for the deep mounting holes' },
  ] }
  panel.workOffsets = null; vi.mocked(send).mockClear()
})
afterEach(() => {
  const element = wrapper?.element as HTMLElement | undefined; wrapper?.unmount(); element?.remove(); wrapper = null
  dispose?.(); dispose = null
})
function mountTools(): void { wrapper = mount(fixture, { attachTo: document.body }) }

describe('tool table workflow', () => {
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
  it('opens editing from a row, keeps model controls inside, and confirms deletion there', async () => {
    mountTools()
    expect(getWrapper().text()).not.toContain('Model defaults')
    expect(getWrapper().findAll('.tool-row button').map((button) => button.text())).toEqual(['Edit', 'Load', 'Edit', 'Load'])
    await getWrapper().get('[aria-label="Edit tool 2"]').trigger('click')
    expect(getWrapper().get('[role="dialog"]').attributes('aria-label')).toBe('Edit tool 2')
    expect(getWrapper().get('[role="dialog"]').text()).toContain('Model defaults')
    await press('Delete'); expect(send).not.toHaveBeenCalled()
    expect(getWrapper().get('[role="dialog"]').attributes('aria-label')).toBe('Remove tool 2?')
    await press('Remove tool'); expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'remove_tool', number: 2 })
  })
  it('cannot delete the spindle tool or overwrite a changed table row', async () => {
    mountTools(); await getWrapper().get('.tool-row').trigger('click')
    const deletion = getWrapper().findAll('button').find((button) => button.text() === 'Delete')
    expect(deletion?.attributes('disabled')).toBeDefined()
    const tool = panel.toolTable?.tools[0]; if (!tool) throw new Error('Missing tool')
    tool.length = 100; await nextTick(); await press('Save to tool table')
    expect(send).not.toHaveBeenCalled()
  })
  it('keeps unchanged length and diameter exact when editing a description in inches', async () => {
    panel.unit = 'inch'
    const tool = panel.toolTable?.tools[0]; if (!tool) throw new Error('Missing tool')
    tool.length = 1.234567891; tool.diameter = 6.123456789
    mountTools(); await getWrapper().get('.tool-row').trigger('click')
    await getWrapper().get('.description-entry input').setValue('Updated description')
    await press('Save to tool table')
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
