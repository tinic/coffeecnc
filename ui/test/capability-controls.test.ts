import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { nextTick } from 'vue'
import ManualTab from '../src/components/ManualTab.vue'
import RunBar from '../src/components/RunBar.vue'
import { panel, send } from '../src/machine'
import { hello, SimMachine } from '../src/sim/machine'

vi.mock('../src/machine', async () => {
  const { computed, reactive } = await import('vue')
  const { requiresHoming } = await import('../src/machine-capabilities')
  const panel = reactive({ hello: null, status: null, link: 'live', unit: 'mm', mdi: [], jogVelocity: 1500, jogIncrement: 1 }) as unknown as typeof import('../src/machine').panel
  return { panel, send: vi.fn(), onResult: (): (() => void) => (): void => undefined,
    axes: computed(() => panel.status?.axes ?? []), reported: computed(() => panel.status),
    live: computed(() => true), manualBlock: computed(() => null), mdiBlock: computed(() => null), linkBlock: computed(() => null),
    allHomed: computed(() => panel.status?.axes.every((axis) => axis.homed) ?? false),
    homingActive: computed(() => panel.status?.axes.some((axis) => axis.homing) ?? false),
    homingRequired: computed(() => requiresHoming(panel.hello)),
  }
})

let wrapper: VueWrapper | null = null
const originalScrollTo = Object.getOwnPropertyDescriptor(HTMLElement.prototype, 'scrollTo')
beforeEach(() => {
  vi.useFakeTimers(); vi.mocked(send).mockClear()
  // jsdom has no scrolling implementation; the MDI history scroll is unrelated to this check.
  Object.defineProperty(HTMLElement.prototype, 'scrollTo', { configurable: true, value: vi.fn() })
  panel.hello = { ...hello, max_jog_velocity: 1500, axis_max_jog_velocity: { x: 1500, y: 1500, z: 750 } }
  panel.status = new SimMachine({ emit: (): void => undefined }).status()
  panel.status.machine = 'on'; panel.status.program.file = '/sim/test.ngc'
  panel.jogVelocity = 1500; panel.jogIncrement = 1
})
afterEach(() => {
  wrapper?.unmount(); vi.useRealTimers()
  if (originalScrollTo === undefined) Reflect.deleteProperty(HTMLElement.prototype, 'scrollTo')
  else Object.defineProperty(HTMLElement.prototype, 'scrollTo', originalScrollTo)
})

describe('capability-aware controls', () => {
  it('sends clamped keyboard step/hold requests and releases a hold on a new hello', async () => {
    wrapper = mount(ManualTab)
    expect(wrapper.text()).toContain('Axis limits: Z 750 mm/min')
    window.dispatchEvent(new KeyboardEvent('keydown', { code: 'PageUp' }))
    window.dispatchEvent(new KeyboardEvent('keyup', { code: 'PageUp' }))
    expect(send).toHaveBeenCalledWith({ type: 'jog_step', axis: 'z', distance: 1, velocity: 750 })
    panel.jogIncrement = null
    window.dispatchEvent(new KeyboardEvent('keydown', { code: 'PageUp' }))
    expect(send).toHaveBeenCalledWith({ type: 'jog', axis: 'z', direction: 1, velocity: 750, held: true })
    panel.hello = { ...hello }
    await nextTick()
    expect(send).toHaveBeenCalledWith(expect.objectContaining({ type: 'jog', axis: 'z', held: false }))
    expect(send).toHaveBeenCalledWith({ type: 'jog_stop_all' })
  })

  it('enables unhomed cycle start only with an explicit capability and blocks active homing', async () => {
    wrapper = mount(RunBar)
    const start = (): ReturnType<NonNullable<typeof wrapper>['get']> => {
      if (wrapper === null) throw new Error('Missing wrapper')
      return wrapper.get('button.main-action')
    }
    expect(start().attributes('disabled')).toBeDefined()
    panel.hello = { ...hello, homing_required: false }
    await nextTick()
    expect(start().attributes('disabled')).toBeUndefined()
    await start().trigger('click')
    expect(send).toHaveBeenCalledWith({ type: 'run' })
    panel.status?.axes.forEach((axis) => { axis.homing = true })
    await nextTick()
    expect(start().attributes('disabled')).toBeDefined()
    expect(start().attributes('title')).toBe('Homing is in progress')
  })
})
