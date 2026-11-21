// The path view draws again only when the picture changes. The status arrives 20 times a second
// even at rest; each one is a new object with the same values.

import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { defineComponent, h, nextTick, watch } from 'vue'

import PathView from '../src/components/PathView.vue'
import { panel } from '../src/machine'
import type { Status, Tool } from '../src/protocol'
import { hello, SimMachine } from '../src/sim/machine'

const sceneDraws = vi.hoisted(() => ({ count: 0 }))

vi.mock('../src/machine', async () => {
  const { computed, reactive } = await import('vue')
  const panel = reactive({ hello: null, status: null, link: 'live', preview: null, program: null, toolTable: null, simulated: false }) as unknown as typeof import('../src/machine').panel
  return { panel, live: computed(() => panel.link === 'live'), send: vi.fn(), onResult: (): (() => void) => (): void => undefined }
})
vi.mock('../src/tool-models', async () => {
  const { reactive } = await import('vue')
  const { factoryToolDefaults, toolIdentity } = await import('../src/tool-model')
  const tool = { number: 4, pocket: 7, diameter: 6, length: 40, comment: '' }
  return { toolModels: reactive({ defaults: factoryToolDefaults(),
    assignments: new Map([[4, { identity: toolIdentity(tool), family: 'end-mill', overrides: {} }]]) }) }
})
// The 3D scene draws whenever its tool changes; count that instead of drawing with WebGL.
vi.mock('../src/components/SceneCanvas.vue', async () => {
  const { defineComponent: define } = await import('vue')
  return { __esModule: true, default: define({
    props: { camera: Object, width: Number, height: Number, tool: Object, probeFixture: Boolean },
    setup(props) {
      watch(() => props.tool, () => { sceneDraws.count += 1 })
      return (): ReturnType<typeof h> => h('div', { class: 'scene' })
    },
  }) }
})

const tool: Tool = { number: 4, pocket: 7, diameter: 6, length: 40, comment: '' }
function status(z = -20): Status {
  const s = new SimMachine({ emit: (): void => undefined }).status()
  s.link = 'live'; s.machine = 'on'
  s.axes.forEach((axis, i) => { axis.homed = true; axis.machine = [100, 50, z][i] ?? 0 })
  s.tool = { in_spindle: 4, prepared: null }
  return s
}

let wrapper: VueWrapper | null = null
let frames = 0
beforeEach(() => {
  frames = 0; sceneDraws.count = 0
  vi.spyOn(window, 'requestAnimationFrame').mockImplementation((callback) => { frames += 1; callback(0); return 0 })
  vi.stubGlobal('ResizeObserver', class { observe(): void { /* Fixed size. */ } disconnect(): void { /* Nothing. */ } })
  panel.hello = hello
  panel.link = 'live'
  panel.toolTable = { tools: [tool] } as typeof panel.toolTable
  panel.status = status()
})
afterEach(() => {
  wrapper?.unmount(); wrapper = null
  vi.restoreAllMocks(); vi.unstubAllGlobals()
})

/** One second of status at rest: twenty new objects with the same values. */
async function idleSecond(z = -20): Promise<void> {
  for (let tick = 0; tick < 20; tick += 1) { panel.status = status(z); await nextTick() }
}

describe('path view redraws', () => {
  it('draws nothing again during a second at rest, and once when the tool moves', async () => {
    wrapper = mount(defineComponent({ components: { PathView }, template: '<PathView/>' }))
    await flushPromises()
    expect(wrapper.find('.scene').exists()).toBe(true)
    frames = 0; sceneDraws.count = 0
    await idleSecond()
    expect({ frames, sceneDraws: sceneDraws.count }).toEqual({ frames: 0, sceneDraws: 0 })
    panel.status = status(-10); await nextTick()
    expect({ frames, sceneDraws: sceneDraws.count }).toEqual({ frames: 1, sceneDraws: 1 })
    await idleSecond(-10)
    expect({ frames, sceneDraws: sceneDraws.count }).toEqual({ frames: 1, sceneDraws: 1 })
  })
})
