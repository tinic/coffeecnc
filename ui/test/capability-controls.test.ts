import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { nextTick } from 'vue'
import ManualTab from '../src/components/ManualTab.vue'
import RunBar from '../src/components/RunBar.vue'
import StatusBar from '../src/components/StatusBar.vue'
import { panel, send } from '../src/machine'
import { hello, SimMachine } from '../src/sim/machine'

vi.mock('../src/machine', async () => {
  const { computed, reactive } = await import('vue')
  const { requiresHoming } = await import('../src/machine-capabilities')
  const panel = reactive({ hello: null, status: null, link: 'live', unit: 'mm', mdi: [], jogVelocity: 1500, jogIncrement: 1, tab: 'manual', notice: null, log: [] }) as unknown as typeof import('../src/machine').panel
  return { panel, send: vi.fn(), dismissNotice: vi.fn(), onResult: (): (() => void) => (): void => undefined,
    axes: computed(() => panel.status?.axes ?? []), reported: computed(() => panel.status),
    live: computed(() => panel.link === 'live'),
    manualBlock: computed(() => panel.link !== 'live' ? 'Link unavailable' :
      panel.status?.machine !== 'on' ? 'Machine unavailable' : panel.status.interp !== 'idle' ? 'Machine busy' : null),
    mdiBlock: computed(() => null), linkBlock: computed(() => null),
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
  panel.link = 'live'
  panel.jogVelocity = 1500; panel.jogIncrement = 1
})
afterEach(() => {
  wrapper?.unmount(); vi.useRealTimers()
  if (originalScrollTo === undefined) Reflect.deleteProperty(HTMLElement.prototype, 'scrollTo')
  else Object.defineProperty(HTMLElement.prototype, 'scrollTo', originalScrollTo)
})

describe('capability-aware controls', () => {
  it('allows an explicit E-stop reset while off without powering on', async () => {
    if (panel.status === null) throw new Error('Missing status')
    panel.status.machine = 'off'
    wrapper = mount(StatusBar)
    const reset = wrapper.findAll('button').find((button) => button.text() === 'Reset E-stop')
    const on = wrapper.findAll('button').find((button) => button.text() === 'Machine on')
    expect(reset).toBeDefined(); expect(on).toBeDefined()
    await reset?.trigger('click')
    expect(vi.mocked(send).mock.calls).toEqual([[{ type: 'set_machine', state: 'off' }]])
    vi.mocked(send).mockClear()
    await on?.trigger('click')
    expect(vi.mocked(send).mock.calls).toEqual([[{ type: 'set_machine', state: 'on' }]])
    panel.status.machine = 'on'
    await nextTick()
    expect(wrapper.text()).not.toContain('Reset E-stop')
    expect(wrapper.text()).toContain('Machine off')
  })

  it.each(['stale', 'silent', 'incompatible'] as const)('blocks reset and power-on with a %s link', async (link) => {
    if (panel.status === null) throw new Error('Missing status')
    panel.status.machine = 'off'; panel.link = link
    wrapper = mount(StatusBar)
    const actions = wrapper.findAll('.actions button').filter((button) => ['Reset E-stop', 'Machine on'].includes(button.text()))
    expect(actions).toHaveLength(2)
    for (const button of actions) {
      expect(button.attributes('disabled')).toBeDefined()
      await button.trigger('click')
    }
    expect(send).not.toHaveBeenCalled()
  })

  it.each(['stale', 'silent', 'off', 'estop', 'busy', 'homing', 'homed'] as const)(
    'ends a held jog on %s and requires a new physical press after recovery', async (interruption) => {
      panel.jogIncrement = null
      wrapper = mount(ManualTab)
      window.dispatchEvent(new KeyboardEvent('keydown', { code: 'PageUp' }))
      vi.advanceTimersByTime(100)
      expect(send).toHaveBeenCalledWith(expect.objectContaining({ type: 'jog', held: true }))
      vi.mocked(send).mockClear()
      const status = panel.status
      if (status === null) throw new Error('Missing status')
      if (interruption === 'stale' || interruption === 'silent') panel.link = interruption
      else if (interruption === 'off' || interruption === 'estop') status.machine = interruption
      else if (interruption === 'busy') status.interp = 'reading'
      else if (interruption === 'homing') status.axes.forEach((axis) => { axis.homing = true })
      else status.axes.forEach((axis) => { axis.homed = true })
      // The stop happens immediately, even if a live snapshot follows in the same event turn.
      expect(send).toHaveBeenCalledWith(expect.objectContaining({ type: 'jog', held: false }))
      expect(send).toHaveBeenCalledWith({ type: 'jog_stop_all' })
      panel.link = 'live'; status.machine = 'on'; status.interp = 'idle'
      status.axes.forEach((axis) => { axis.homing = false })
      await nextTick()
      vi.mocked(send).mockClear()
      window.dispatchEvent(new KeyboardEvent('keydown', { code: 'PageUp', repeat: true }))
      vi.advanceTimersByTime(500)
      expect(send).not.toHaveBeenCalled()
      window.dispatchEvent(new KeyboardEvent('keyup', { code: 'PageUp' }))
      window.dispatchEvent(new KeyboardEvent('keydown', { code: 'PageUp' }))
      expect(send).toHaveBeenCalledWith(expect.objectContaining({ type: 'jog', held: true }))
    },
  )

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
