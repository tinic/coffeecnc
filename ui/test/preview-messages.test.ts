import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { shallowMount, type VueWrapper } from '@vue/test-utils'
import { nextTick } from 'vue'
import PathView from '../src/components/PathView.vue'
import { panel } from '../src/machine'
import { SimMachine } from '../src/sim/machine'

vi.mock('../src/machine', async () => {
  const { computed, reactive } = await import('vue')
  const panel = reactive({}) as typeof import('../src/machine').panel
  return { panel, live: computed(() => true) }
})
vi.mock('../src/appearance', async () => ({ theme: (await import('vue')).ref('light') }))
vi.mock('../src/step', () => ({ part: { attachment: null }, showPart: false, partBlock: '' }))
vi.mock('../src/tool-models', () => ({ toolModels: { assignments: {}, defaults: {} } }))
vi.mock('../src/tool-preview', () => ({
  loadedToolPreview: (): null => null,
  reportedToolTip: (): null => null,
  toolPreviewBounds: vi.fn(),
}))

let wrapper: VueWrapper | null = null
beforeEach(() => {
  panel.link = 'live'
  panel.status = new SimMachine({ emit: (): void => undefined }).status()
  panel.status.program.file = '/parts/a.ngc'
  panel.preview = { type: 'preview', context: panel.status.preview_context,
    state: 'pending', error: null, segments: [], bounds: null }
  vi.stubGlobal('ResizeObserver', class {
    observe(): void { /* Layout is not measured in these message tests. */ }
    disconnect(): void { /* No observer was scheduled. */ }
  })
  vi.spyOn(window, 'requestAnimationFrame').mockReturnValue(1)
  vi.spyOn(window, 'cancelAnimationFrame').mockImplementation(() => undefined)
})
afterEach(() => { wrapper?.unmount(); vi.restoreAllMocks(); vi.unstubAllGlobals() })

describe('preview messages', () => {
  it('only calls a pending preview current when its context matches', async () => {
    wrapper = shallowMount(PathView)
    expect(wrapper.get('.banners').text()).toContain('Computing preview')
    if (panel.status === null) throw new Error('Missing fixture')
    panel.status.preview_context += 1
    await nextTick()
    expect(wrapper.get('.banners').text()).toContain('Preview out of date')
    expect(wrapper.get('.banners').text()).not.toContain('Computing preview')
  })

  it('does not invent line zero or claim an empty failed preview contains a path', () => {
    if (panel.preview === null) throw new Error('Missing fixture')
    panel.preview.state = 'failed'; panel.preview.error = { line: 0, text: 'Timed out.' }
    wrapper = shallowMount(PathView)
    expect(wrapper.get('.banners').text()).toContain('Preview stopped: no path available')
    expect(wrapper.get('.banners').text()).not.toContain('line 0')
    expect(wrapper.get('.banners').text()).not.toContain('path shown')
  })

  it('identifies an incomplete path at a known source line', () => {
    if (panel.preview === null) throw new Error('Missing fixture')
    panel.preview.state = 'failed'; panel.preview.error = { line: 3, text: 'Unsupported operation.' }
    panel.preview.segments = [{ line: 2, kind: 'feed', tool: 1, points: [0, 0, 0, 1, 0, 0] }]
    wrapper = shallowMount(PathView)
    expect(wrapper.get('.banners').text()).toContain('Preview stopped at line 3: partial path shown')
  })

  it('distinguishes an unavailable preview, no loaded program and an obsolete context', async () => {
    if (panel.preview === null || panel.status === null) throw new Error('Missing fixture')
    panel.preview.state = 'unavailable'
    wrapper = shallowMount(PathView)
    expect(wrapper.get('.banners').text()).toContain('Preview unavailable')
    panel.status.program.file = null
    await nextTick()
    expect(wrapper.get('.banners').text()).toContain('No program loaded')
    panel.status.preview_context += 1
    await nextTick()
    expect(wrapper.get('.banners').text()).toContain('Preview out of date')
  })
})
