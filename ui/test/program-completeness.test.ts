import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { shallowMount, type VueWrapper } from '@vue/test-utils'
import { nextTick } from 'vue'
import FileBrowser from '../src/components/FileBrowser.vue'
import ProgramTab from '../src/components/ProgramTab.vue'
import { panel } from '../src/machine'
import { SimMachine } from '../src/sim/machine'

vi.mock('../src/machine', async () => {
  const { computed, reactive } = await import('vue')
  const panel = reactive({}) as typeof import('../src/machine').panel
  return { panel, send: vi.fn(), findTool: (): undefined => undefined,
    live: computed(() => true), reported: computed(() => panel.status) }
})

let wrapper: VueWrapper | null = null
beforeEach(() => {
  panel.status = new SimMachine({ emit: (): void => undefined }).status()
  panel.status.program = { ...panel.status.program, file: '/parts/a.ngc', revision: 3, current_line: 200 }
  panel.program = { type: 'program', file: '/parts/a.ngc', revision: 3, lines: ['G0 X1', 'G0 Y2'], truncated: true }
  panel.directory = { type: 'directory', path: '/parts', parent: null, entries: [], truncated: true }
  panel.preview = null; panel.programLayout = 'text'; panel.unit = 'mm'
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
