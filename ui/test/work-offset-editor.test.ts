import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { defineComponent, nextTick } from 'vue'
import ToolsTab from '../src/components/ToolsTab.vue'
import { panel, send } from '../src/machine'
import { hello, SimMachine } from '../src/sim/machine'
import type { Status, WorkOffsets } from '../src/protocol'

vi.mock('../src/machine', async () => {
  const { reactive, computed } = await import('vue')
  const panel = reactive({ hello: null, toolModels: null, simulated: true }) as typeof import('../src/machine').panel
  const linkBlock = computed(() => panel.link !== 'live' || panel.status?.link !== 'live' ? 'Connection unavailable' : null)
  return { panel, linkBlock,
    mdiBlock: computed(() => linkBlock.value ?? (panel.status?.machine !== 'on' ? 'Machine is off'
      : panel.status.interp !== 'idle' ? 'Machine is busy' : null)),
    findTool: (): null => null, send: vi.fn(), onResult: (): (() => void) => () => undefined }
})
const fixture = defineComponent({ components: { ToolsTab },
  template: '<div><ToolsTab/><div id="dialogs"></div><div id="workspace-dialogs"></div></div>' })
let wrapper: VueWrapper | null = null
function status(): Status { if (!panel.status) throw new Error('Missing status'); return panel.status }
function origin(index: number): WorkOffsets['systems'][number] {
  const item = panel.workOffsets?.systems[index]; if (!item) throw new Error('Missing origin'); return item
}
function view(): VueWrapper { if (!wrapper) throw new Error('Missing wrapper'); return wrapper }
async function press(text: string): Promise<void> {
  const button = view().findAll('button').find((item) => item.text() === text)
  if (!button) throw new Error(`Missing button: ${text}`)
  await button.trigger('click')
}
async function open(wcs = 1): Promise<void> {
  wrapper = mount(fixture, { attachTo: document.body })
  await view().findAll('.offset-row')[wcs - 1]?.trigger('click')
}
beforeEach(() => {
  panel.hello = { ...hello }; panel.link = 'live'; panel.unit = 'mm'; panel.toolTable = null
  panel.status = new SimMachine({ emit: (): void => undefined }).status()
  panel.status.machine = 'on'; panel.status.link = 'live'; panel.status.offsets.wcs = 1
  panel.workOffsets = { type: 'work_offsets', revision: 1, systems: Array.from({ length: 9 }, (_, index) => ({
    wcs: index + 1, available: true, offset: [25.4, -50.8, 3.5], rotation_xy: 15,
  })) }
  vi.mocked(send).mockClear()
})
afterEach(() => { const element = wrapper?.element as HTMLElement | undefined; wrapper?.unmount(); element?.remove(); wrapper = null })

describe('confirmed work origins', () => {
  it.each(['mm', 'inch'] as const)('edits %s formulas without selecting the system, then confirms in canonical mm', async (unit) => {
    panel.unit = unit; await open(2)
    expect(send).not.toHaveBeenCalled()
    await view().get('[aria-label="G55 X origin"]').setValue('1/4in')
    await view().get('[aria-label="G55 Y origin"]').setValue('7mm/2')
    await view().get('[aria-label="G55 Z origin"]').setValue('7/2mm')
    await press('Review changes')
    expect(view().get('[role="dialog"]').attributes('aria-label')).toBe('Set XYZ in G55?')
    expect(view().text()).toContain('XY rotation and G92/G52 shifts stay unchanged')
    expect(send).not.toHaveBeenCalled()
    await press('Confirm G55 XYZ')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'set_work_offset', wcs: 2, offset: [6.35, 3.5, 3.5] })
    expect(view().find('[role="dialog"]').exists()).toBe(false)
  })
  it('keeps selection separate from opening the editor', async () => {
    wrapper = mount(fixture, { attachTo: document.body })
    await view().findAll('.offset-row')[1]?.get('button').trigger('click')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'select_wcs', wcs: 2 })
    expect(view().find('[role="dialog"]').exists()).toBe(false)
  })
  it('copies entered XYZ to the chosen destination only after review', async () => {
    wrapper = mount(fixture, { attachTo: document.body })
    await view().get('[aria-label="Edit G54 origins"]').trigger('click')
    await view().get('[aria-label="G54 Z origin"]').setValue('-1/4in')
    await view().get('[aria-label="Copy destination"]').setValue(9)
    await press('Copy XYZ…')
    expect(view().get('[role="dialog"]').attributes('aria-label')).toBe('Copy XYZ to G59.3?')
    expect(send).not.toHaveBeenCalled(); await press('Confirm G59.3 XYZ')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'set_work_offset', wcs: 9, offset: [25.4, -50.8, -6.35] })
  })
  it('clears XYZ only after confirmation and allows returning to the unchanged draft', async () => {
    await open(); await view().get('[aria-label="G54 X origin"]').setValue('12/2')
    await press('Clear XYZ…'); expect(send).not.toHaveBeenCalled(); await press('Back')
    expect((view().get('input').element as HTMLInputElement).value).toBe('12/2')
    await press('Clear XYZ…'); await press('Confirm G54 XYZ')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'set_work_offset', wcs: 1, offset: [0, 0, 0] })
  })
  it('never invents zero for unknown origins and requires three explicit entries', async () => {
    origin(1).available = false
    await open(2)
    expect(view().findAll('input').map((field) => (field.element as HTMLInputElement).value)).toEqual(['', '', ''])
    await press('Review changes'); expect(view().find('.changes').exists()).toBe(false)
    await view().get('[aria-label="G55 X origin"]').setValue('1')
    await view().get('[aria-label="G55 Y origin"]').setValue('2')
    await press('Copy XYZ…'); expect(view().find('.changes').exists()).toBe(false)
    await view().get('[aria-label="G55 Z origin"]').setValue('3')
    await press('Review changes'); expect(view().findAll('.changes td').filter((cell) => cell.text() === 'Unavailable')).toHaveLength(3)
    await press('Confirm G55 XYZ')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'set_work_offset', wcs: 2, offset: [1, 2, 3] })
  })
  it('retains exact unedited origins in inches across encoder feedback and status replacement', async () => {
    panel.unit = 'inch'; origin(0).offset = [1.234567891, -2.345678912, 3.456789123]
    await open(); await press('Review changes')
    panel.status = { ...status(), axes: status().axes.map((axis) => ({ ...axis, work: axis.work + .001 })) }
    await nextTick(); expect(view().find('.changes').exists()).toBe(true)
    await press('Confirm G54 XYZ')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'set_work_offset', wcs: 1, offset: [1.234567891, -2.345678912, 3.456789123] })
  })
  it.each(['source', 'destination'] as const)('blocks a review if the %s origins change', async (which) => {
    await open(); await press('Copy XYZ…')
    origin(which === 'source' ? 0 : 1).offset = [4, 5, 6]
    await nextTick(); await press('Confirm G55 XYZ')
    expect(send).not.toHaveBeenCalled(); expect(view().get('.blocked').text()).toContain('changed')
  })
  it.each(['off', 'busy', 'stale', 'system'] as const)('cancels confirmation on %s changes but retains the draft', async (change) => {
    await open(); await view().get('input').setValue('1/4in'); await press('Review changes')
    if (change === 'off') status().machine = 'off'
    if (change === 'busy') status().interp = 'reading'
    if (change === 'stale') status().link = 'stale'
    if (change === 'system') status().offsets.wcs = 2
    await nextTick(); expect(view().find('.changes').exists()).toBe(false)
    expect((view().get('input').element as HTMLInputElement).value).toBe('1/4in')
    expect(send).not.toHaveBeenCalled()
  })
  it.each(['units', 'reconnect'] as const)('closes on %s changes without issuing a command', async (change) => {
    await open(); if (change === 'units') panel.unit = 'inch'; else panel.hello = { ...hello }
    await nextTick(); expect(view().find('[role="dialog"]').exists()).toBe(false); expect(send).not.toHaveBeenCalled()
  })
})
