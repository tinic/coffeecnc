import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { nextTick } from 'vue'
import ToolsTab from '../src/components/ToolsTab.vue'
import { mdiBlock, panel, send } from '../src/machine'
import type { WorkOffsets } from '../src/protocol'
import { SimMachine } from '../src/sim/machine'

vi.mock('../src/machine', async () => {
  const { reactive, ref } = await import('vue')
  return {
    panel: reactive({ hello: null, toolModels: null, simulated: true }) as typeof import('../src/machine').panel,
    findTool: (): null => null,
    linkBlock: ref(null),
    mdiBlock: ref(null),
    send: vi.fn(),
    onResult: (): (() => void) => () => undefined,
  }
})

let wrapper: VueWrapper | null = null
const offsets = (): WorkOffsets => ({
  type: 'work_offsets', revision: 6,
  systems: Array.from({ length: 9 }, (_, index) => ({
    wcs: index + 1, available: index === 1,
    offset: index === 1 ? [100, 100, -100] : [12345, 23456, 34567],
    rotation_xy: index === 1 ? 15 : 90,
  })),
})
const rows = (): VueWrapper['element'][] => wrapper?.findAll('.offsets tbody tr').map((row) => row.element) ?? []
beforeEach(() => {
  panel.status = new SimMachine({ emit: (): void => undefined }).status()
  panel.status.offsets.wcs = 2
  panel.status.link = 'live'
  panel.link = 'live'
  panel.unit = 'mm'
  panel.toolTable = null
  panel.workOffsets = offsets()
  vi.mocked(send).mockClear()
  Object.assign(mdiBlock, { value: null })
})
afterEach(() => { wrapper?.unmount(); wrapper = null })

describe('work offset availability in the operator table', () => {
  it('hides unavailable placeholders, including rotation, while preserving real active values', () => {
    wrapper = mount(ToolsTab)
    expect(rows()).toHaveLength(9)
    const unknown = wrapper.get('.offsets tbody tr')
    expect(unknown.text()).toContain('Unavailable')
    expect(unknown.findAll('td.num').map((cell) => cell.text())).toEqual(['—', '—', '—', '—'])
    expect(wrapper.text()).not.toContain('12345')
    expect(wrapper.text()).not.toContain('90°')
    const active = wrapper.get('.offsets tr.loaded')
    expect(active.text()).toContain('G55')
    expect(active.findAll('td.num').map((cell) => cell.text())).toEqual(['100.000', '100.000', '-100.000', '15°'])
  })

  it('keeps older complete tables and actual zeros readable', () => {
    for (const system of panel.workOffsets?.systems ?? []) {
      delete system.available
      system.offset = [0, 0, 0]
      system.rotation_xy = 0
    }
    wrapper = mount(ToolsTab)
    expect(wrapper.get('.offsets tbody tr').findAll('td.num').map((cell) => cell.text())).toEqual(['0.000', '0.000', '0.000'])
    expect(wrapper.get('.offsets').text()).not.toContain('Unavailable')
    expect(wrapper.get('.offsets thead').text()).not.toContain('Rot')
  })

  it('changes availability with the active system and sends selection only on a click', async () => {
    wrapper = mount(ToolsTab)
    expect(send).not.toHaveBeenCalled()
    await wrapper.get('.offsets tbody tr button').trigger('click')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'select_wcs', wcs: 1 })
    if (panel.status === null || panel.workOffsets === null) throw new Error('Missing fixture')
    panel.status.offsets.wcs = 1
    panel.workOffsets.systems = panel.workOffsets.systems.map((system) => ({ ...system, available: system.wcs === 1, offset: [1, 2, 3], rotation_xy: 0 }))
    await nextTick()
    expect(wrapper.get('.offsets tr.loaded').text()).toContain('G54')
    expect(wrapper.findAll('.offsets tbody tr')[1]?.text()).toContain('Unavailable')
    expect(send).toHaveBeenCalledTimes(1)
  })

  it.each(['stale', 'silent'] as const)('marks retained %s readings as history', (link) => {
    panel.link = link
    Object.assign(mdiBlock, { value: 'No current controller status' })
    wrapper = mount(ToolsTab)
    expect(wrapper.findAll('.offsets tbody tr')[1]?.text()).toContain('Stale')
    expect(wrapper.find('.offsets tr.loaded').exists()).toBe(false)
    expect(wrapper.findAll('.offsets button').every((button) => button.attributes('disabled') !== undefined)).toBe(true)
  })

  it.each(['down', 'connecting', 'incompatible'] as const)('withholds values on a %s connection', (link) => {
    panel.link = link
    wrapper = mount(ToolsTab)
    expect(wrapper.findAll('.offsets tbody td.num').every((cell) => cell.text() === '—')).toBe(true)
  })

  it('respects native stale/down status before the panel link catches up', async () => {
    if (panel.status === null) throw new Error('Missing fixture')
    panel.status.link = 'stale'
    wrapper = mount(ToolsTab)
    expect(wrapper.get('.offsets').text()).toContain('Stale')
    expect(wrapper.findAll('.offsets button').every((button) => button.attributes('disabled') !== undefined)).toBe(true)
    panel.status.link = 'down'
    await nextTick()
    expect(wrapper.findAll('.offsets tbody td.num').every((cell) => cell.text() === '—')).toBe(true)
  })

  it('withholds duplicate, missing and nonfinite rows', () => {
    if (panel.workOffsets === null) throw new Error('Missing fixture')
    panel.workOffsets.systems = [
      { wcs: 1, available: true, offset: [0, 0, 0], rotation_xy: 0 },
      { wcs: 1, available: true, offset: [1, 2, 3], rotation_xy: 0 },
      { wcs: 2, available: true, offset: [NaN, 0, 0], rotation_xy: 0 },
      { wcs: 3, available: true, offset: [0, 0, 0], rotation_xy: Infinity },
    ]
    wrapper = mount(ToolsTab)
    expect(rows()).toHaveLength(9)
    expect(wrapper.findAll('.offsets tbody td.num').every((cell) => cell.text() === '—')).toBe(true)
  })
})
