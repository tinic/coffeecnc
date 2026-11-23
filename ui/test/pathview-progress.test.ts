// The path view's program progress, travel box and pointer bookkeeping, drawn into a 2D context
// that records each stroke.

import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { nextTick } from 'vue'

import PathView from '../src/components/PathView.vue'
import { panel } from '../src/machine'
import { travelBounds } from '../src/pathview'
import type { Preview, Status } from '../src/protocol'
import { hello, SimMachine } from '../src/sim/machine'

vi.mock('../src/machine', async () => {
  const { computed, reactive } = await import('vue')
  const panel = reactive({ hello: null, status: null, link: 'live', preview: null, program: null, toolTable: null, simulated: false }) as unknown as typeof import('../src/machine').panel
  return { panel, live: computed(() => panel.link === 'live'), send: vi.fn(), onResult: (): (() => void) => (): void => undefined }
})

interface Stroke { lineWidth: number; strokeStyle: string }
let strokes: Stroke[] = []

function fakeContext(): CanvasRenderingContext2D {
  const state: Record<string, unknown> = { lineWidth: 1, strokeStyle: '' }
  return new Proxy(state, {
    get(target, key: string): unknown {
      if (key in target) return target[key]
      if (key === 'stroke') return (): void => { strokes.push({ lineWidth: Number(target['lineWidth']), strokeStyle: String(target['strokeStyle']) }) }
      if (key === 'measureText') return (): { width: number } => ({ width: 10 })
      return (): void => undefined
    },
    set(target, key: string, value: unknown): boolean { target[key] = value; return true },
  }) as unknown as CanvasRenderingContext2D
}

/** Feed moves on lines 1 to 3 of the program. */
function preview(context: number): Preview {
  const segment = (line: number): Preview['segments'][number] =>
    ({ line, kind: 'feed', tool: 1, feed: 600, points: [line * 10, 0, -1, line * 10 + 10, 0, -1] })
  return { type: 'preview', context, state: 'ready', error: null, segments: [segment(1), segment(2), segment(3)],
    bounds: { min: [10, 0, -1], max: [40, 0, -1] }, dwells: [] }
}

function running(current: number, motion: number): Status {
  const s = new SimMachine({ emit: (): void => undefined }).status()
  Object.assign(s, { link: 'live', machine: 'on', interp: 'reading', mode: 'auto' })
  s.program = { ...s.program, file: '/programs/test.ngc', revision: 1, current_line: current, motion_line: motion }
  return s
}

const doneWidth = 2.6
let wrapper: VueWrapper | null = null
beforeEach(() => {
  strokes = []
  vi.spyOn(window, 'requestAnimationFrame').mockImplementation((callback) => { callback(0); return 0 })
  vi.stubGlobal('ResizeObserver', class { observe(): void { /* Fixed size. */ } disconnect(): void { /* Nothing. */ } })
  vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockImplementation(() => fakeContext())
  vi.spyOn(HTMLElement.prototype, 'clientWidth', 'get').mockReturnValue(800)
  vi.spyOn(HTMLElement.prototype, 'clientHeight', 'get').mockReturnValue(500)
  HTMLElement.prototype.setPointerCapture = (): void => undefined
  panel.hello = hello
  panel.link = 'live'
})
afterEach(() => {
  wrapper?.unmount(); wrapper = null
  vi.restoreAllMocks(); vi.unstubAllGlobals()
})

async function show(status: Status): Promise<void> {
  panel.status = status
  panel.preview = preview(status.preview_context)
  wrapper = mount(PathView)
  await flushPromises()
}

describe('path view progress', () => {
  it('marks nothing cut while the interpreter reads ahead of the first motion', async () => {
    await show(running(3, 0))
    expect(strokes.filter((stroke) => stroke.lineWidth === doneWidth)).toEqual([])
  })

  it('marks the lines up to the motion line cut', async () => {
    await show(running(1, 1))
    strokes = []
    panel.status = running(3, 2)
    await flushPromises()
    expect(strokes.filter((stroke) => stroke.lineWidth === doneWidth)).toHaveLength(2)
  })
})

describe('machine travel box', () => {
  it('needs valid limits on all three axes', () => {
    const axis = (min: number, max: number): { min_limit: number; max_limit: number } => ({ min_limit: min, max_limit: max })
    expect(travelBounds([axis(0, 400), axis(0, 250), axis(-200, 0)], [0, 0, 50])).toEqual({ min: [0, 0, -250], max: [400, 250, -50] })
    expect(travelBounds([axis(0, 400), axis(0, 0), axis(-200, 0)], [0, 0, 0])).toBeNull()
    expect(travelBounds([axis(0, 400), axis(0, 250), axis(0, NaN)], [0, 0, 0])).toBeNull()
    expect(travelBounds([axis(0, 400), axis(0, 250)], [0, 0, 0])).toBeNull()
  })
})

describe('path view pointers', () => {
  it('forgets a pointer whose capture is lost, so the next drag orbits', async () => {
    await show(running(0, 0))
    const canvas = wrapper?.find('canvas.toolpath-canvas')
    if (!canvas) throw new Error('No canvas')
    const iso = wrapper?.findAll('button').find((button) => button.text() === 'Iso')
    expect(iso?.attributes('aria-pressed')).toBe('true')
    const pointer = (type: string, pointerId: number, x = 0, y = 0): void => {
      const event = new MouseEvent(type, { clientX: x, clientY: y, bubbles: true })
      Object.defineProperty(event, 'pointerId', { value: pointerId })
      canvas.element.dispatchEvent(event)
    }
    pointer('pointerdown', 1, 100, 100)
    pointer('lostpointercapture', 1)
    pointer('pointerdown', 2, 200, 200)
    pointer('pointermove', 2, 260, 200)
    await nextTick()
    expect(iso?.attributes('aria-pressed')).toBe('false')
  })

  it('reports the line of a tapped move, and not for a drag or a pinch', async () => {
    await show(running(0, 0))
    const canvas = wrapper?.find('canvas.toolpath-canvas')
    if (!canvas || !wrapper) throw new Error('No canvas')
    const iso = wrapper.findAll('button').find((button) => button.text() === 'Iso')
    const pointer = (type: string, pointerId: number, x: number, y: number): void => {
      const event = new MouseEvent(type, { clientX: x, clientY: y, bubbles: true })
      Object.defineProperty(event, 'pointerId', { value: pointerId })
      canvas.element.dispatchEvent(event)
    }
    // The fitted path is centred: the middle of the canvas is on the move of line 2.
    pointer('pointerdown', 1, 400, 250)
    pointer('pointermove', 1, 404, 253)
    pointer('pointerup', 1, 404, 253)
    expect(wrapper.emitted('pick')).toEqual([[2]])
    expect(iso?.attributes('aria-pressed')).toBe('true')
    // Beside the path: no move.
    pointer('pointerdown', 1, 20, 20)
    pointer('pointerup', 1, 20, 20)
    expect(wrapper.emitted('pick')?.at(-1)).toEqual([0])
    // A drag orbits and picks nothing.
    pointer('pointerdown', 1, 400, 250)
    pointer('pointermove', 1, 460, 250)
    pointer('pointerup', 1, 400, 250)
    await nextTick()
    expect(iso?.attributes('aria-pressed')).toBe('false')
    // Two fingers that hardly move are a pinch, not a tap.
    pointer('pointerdown', 1, 400, 250)
    pointer('pointerdown', 2, 420, 250)
    pointer('pointerup', 2, 420, 250)
    pointer('pointerup', 1, 400, 250)
    expect(wrapper.emitted('pick')).toHaveLength(2)
  })

  it('draws the moves of highlighted lines on top', async () => {
    await show(running(0, 0))
    expect(strokes.filter((stroke) => stroke.lineWidth === 3)).toEqual([])
    strokes = []
    await wrapper?.setProps({ highlight: { first: 2, last: 3 } })
    await flushPromises()
    const halo = strokes.findIndex((stroke) => stroke.lineWidth === 6 && stroke.strokeStyle === '#ffffff')
    expect(halo).toBeGreaterThan(0)
    expect(strokes[halo + 1]).toEqual({ lineWidth: 3, strokeStyle: '#d9480f' })
    // After every move of the path: the planned moves are the first three strokes.
    expect(strokes.slice(0, 3).every((stroke) => stroke.lineWidth === 1.55)).toBe(true)
  })
})
