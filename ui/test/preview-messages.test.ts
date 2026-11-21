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
    state: 'pending', error: null, segments: [], bounds: null, dwells: [] }
  vi.stubGlobal('ResizeObserver', class {
    observe(): void { /* Layout is not measured in these message tests. */ }
    disconnect(): void { /* No observer was scheduled. */ }
  })
  vi.spyOn(window, 'requestAnimationFrame').mockReturnValue(1)
  vi.spyOn(window, 'cancelAnimationFrame').mockImplementation(() => undefined)
})
afterEach(() => { wrapper?.unmount(); vi.useRealTimers(); vi.restoreAllMocks(); vi.unstubAllGlobals() })

describe('preview messages', () => {
  it('hides a brief recomputation and delays a current pending notice for one second', async () => {
    vi.useFakeTimers()
    if (panel.status === null || panel.preview === null) throw new Error('Missing fixture')
    wrapper = shallowMount(PathView)
    expect(wrapper.find('.preview-banner').exists()).toBe(false)
    await vi.advanceTimersByTimeAsync(500)
    panel.preview = { ...panel.preview, state: 'ready' }
    await nextTick()
    await vi.advanceTimersByTimeAsync(500)
    expect(wrapper.find('.preview-banner').exists()).toBe(false)

    panel.preview = { ...panel.preview, state: 'pending' }
    await nextTick()
    await vi.advanceTimersByTimeAsync(999)
    expect(wrapper.find('.preview-banner').exists()).toBe(false)
    await vi.advanceTimersByTimeAsync(1)
    expect(wrapper.get('.preview-banner').text()).toBe('Computing preview')

    panel.status.preview_context += 1
    await nextTick()
    expect(wrapper.get('.preview-banner').text()).toBe('Preview out of date')
  })

  it('keeps the drawn path until the next preview for the same file', async () => {
    vi.useFakeTimers()
    if (panel.status === null) throw new Error('Missing status')
    const context = panel.status.preview_context
    panel.preview = { type: 'preview', context, state: 'ready',
      error: null, bounds: null, dwells: [], segments: [{ line: 2, kind: 'feed', tool: 1, points: [0, 0, 0, 1, 0, 0], feed: 100 }] }
    wrapper = shallowMount(PathView)
    expect(wrapper.classes()).not.toContain('outdated')

    panel.status.preview_context += 1
    panel.preview = { type: 'preview', context: context + 1, state: 'unavailable',
      error: { line: 0, text: 'The preview updates when the run ends.' }, bounds: null, dwells: [], segments: [] }
    await nextTick()
    expect(wrapper.classes()).toContain('outdated')
    expect(wrapper.get('.preview-banner').text()).toContain('The preview updates when the run ends.')

    panel.preview = { ...panel.preview, state: 'pending', error: null }
    await nextTick()
    expect(wrapper.classes()).toContain('outdated')
    expect(wrapper.find('.preview-banner').exists()).toBe(false)
    await vi.advanceTimersByTimeAsync(1000)
    expect(wrapper.get('.preview-banner').text()).toContain('Computing preview')
    expect(wrapper.find('.banners .preview-banner').exists()).toBe(false)

    panel.status.program.revision += 1
    await nextTick()
    expect(wrapper.classes()).not.toContain('outdated')
  })

  it('does not invent line zero or claim an empty failed preview contains a path', () => {
    if (panel.preview === null) throw new Error('Missing fixture')
    panel.preview.state = 'failed'; panel.preview.error = { line: 0, text: 'Timed out.' }
    wrapper = shallowMount(PathView)
    expect(wrapper.get('.preview-banner').text()).toContain('Preview stopped: no path available')
    expect(wrapper.get('.preview-banner').text()).not.toContain('line 0')
    expect(wrapper.get('.preview-banner').text()).not.toContain('path shown')
  })

  it('identifies an incomplete path at a known source line', () => {
    if (panel.preview === null) throw new Error('Missing fixture')
    panel.preview.state = 'failed'; panel.preview.error = { line: 3, text: 'Unsupported operation.' }
    panel.preview.segments = [{ line: 2, kind: 'feed', tool: 1, points: [0, 0, 0, 1, 0, 0], feed: 100 }]
    wrapper = shallowMount(PathView)
    expect(wrapper.get('.preview-banner').text()).toContain('Preview stopped at line 3: partial path shown')
  })

  it('distinguishes an unavailable preview, no loaded program and an obsolete context', async () => {
    if (panel.preview === null || panel.status === null) throw new Error('Missing fixture')
    panel.preview.state = 'unavailable'
    wrapper = shallowMount(PathView)
    expect(wrapper.get('.preview-banner').text()).toContain('Preview unavailable')
    panel.status.program.file = null
    await nextTick()
    expect(wrapper.get('.preview-banner').text()).toContain('No program loaded')
    panel.status.preview_context += 1
    await nextTick()
    expect(wrapper.get('.preview-banner').text()).toContain('Preview out of date')
  })
})
