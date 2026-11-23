// The probe guide animation asks for frames only while it plays.

import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { nextTick } from 'vue'

import ProbeGuideCanvas from '../src/components/ProbeGuideCanvas.vue'
import { probeGuide } from '../src/probe-guide'

const renders = vi.hoisted(() => ({ count: 0 }))
vi.mock('three', async (original) => ({
  ...await original<typeof import('three')>(),
  WebGLRenderer: class {
    shadowMap = { enabled: false, type: 0 }
    extensions = { has: (): boolean => false }
    setClearColor(): void { /* Nothing to clear. */ }
    setPixelRatio(): void { /* Fixed. */ }
    setSize(): void { /* Fixed. */ }
    render(): void { renders.count += 1 }
    dispose(): void { /* Nothing held. */ }
    forceContextLoss(): void { /* No context. */ }
  },
}))

let frames: FrameRequestCallback[] = []
let wrapper: VueWrapper | null = null
beforeEach(() => {
  frames = []
  renders.count = 0
  vi.spyOn(window, 'requestAnimationFrame').mockImplementation((callback) => { frames.push(callback); return frames.length })
  vi.spyOn(window, 'cancelAnimationFrame').mockImplementation(() => undefined)
  vi.stubGlobal('ResizeObserver', class { observe(): void { /* Fixed size. */ } disconnect(): void { /* Nothing. */ } })
})
afterEach(() => {
  wrapper?.unmount(); wrapper = null
  vi.restoreAllMocks(); vi.unstubAllGlobals()
})

/** Runs the frames asked for so far; returns how many there were. */
function runFrames(now: number): number {
  const due = frames
  frames = []
  for (const callback of due) callback(now)
  return due.length
}

describe('probe guide canvas', () => {
  it('asks for no frames while paused, and again once it plays', async () => {
    const plan = probeGuide({ kind: 'teach' }, undefined, 'y_plus', null)
    wrapper = mount(ProbeGuideCanvas, { props: { plan, playing: false, ruby: false } })
    expect(wrapper.emitted('unavailable')).toBeUndefined()
    runFrames(16)
    expect(frames).toEqual([])
    await wrapper.setProps({ playing: true })
    await nextTick()
    const before = renders.count
    expect(runFrames(32)).toBe(1)
    expect(runFrames(48)).toBe(1)
    expect(renders.count).toBeGreaterThan(before)
    await wrapper.setProps({ playing: false })
    runFrames(64)
    expect(frames).toEqual([])
  })

  it('draws nothing and asks for no frames once unmounted mid-animation (#279)', () => {
    const plan = probeGuide({ kind: 'teach' }, undefined, 'y_plus', null)
    wrapper = mount(ProbeGuideCanvas, { props: { plan, playing: true, ruby: false } })
    expect(runFrames(16)).toBe(1)
    expect(frames).toHaveLength(1)
    const cancel = vi.spyOn(window, 'cancelAnimationFrame').mockImplementation(() => undefined)
    wrapper.unmount(); wrapper = null
    expect(cancel).toHaveBeenCalledWith(frames.length)
    // The mocked cancel leaves the frame queued, as a frame already dispatched would be.
    const before = renders.count
    runFrames(32)
    expect(renders.count).toBe(before)
    expect(frames).toEqual([])
  })
})
