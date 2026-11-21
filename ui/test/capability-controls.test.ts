import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { defineComponent, nextTick } from 'vue'
import ManualTab from '../src/components/ManualTab.vue'
import RunBar from '../src/components/RunBar.vue'
import StatusBar from '../src/components/StatusBar.vue'
import { panel, send } from '../src/machine'
import { touchInput } from '../src/numeric-input'
import { hello, SimMachine } from '../src/sim/machine'

vi.mock('../src/machine', async () => {
  const { computed, reactive } = await import('vue')
  const { defaultOperatorPreferences } = await import('../src/operator-settings')
  const { machineBlock, connectionBlock } = await import('../src/machine-block')
  const { requiresHoming } = await import('../src/machine-capabilities')
  const panel = reactive({ preferences: defaultOperatorPreferences(), startupSpindleRpm: 0, hello: null, status: null, link: 'live', unit: 'mm', mdi: [], jogVelocity: 1500, jogIncrement: 1, tab: 'manual', notice: null, log: [] }) as unknown as typeof import('../src/machine').panel
  return { panel, send: vi.fn(), dismissNotice: vi.fn(), onResult: (): (() => void) => (): void => undefined,
    axes: computed(() => panel.status?.axes ?? []), reported: computed(() => panel.status),
    live: computed(() => panel.link === 'live'),
    manualBlock: computed(() => machineBlock(panel)),
    mdiBlock: computed(() => machineBlock(panel) ?? (requiresHoming(panel.hello) &&
      panel.status?.axes.some((axis) => !axis.homed) ? 'Axes are not homed' : null)),
    linkBlock: computed(() => connectionBlock(panel.link)),
    allHomed: computed(() => panel.status?.axes.every((axis) => axis.homed) ?? false),
    homingActive: computed(() => panel.status?.axes.some((axis) => axis.homing) ?? false),
    homingRequired: computed(() => requiresHoming(panel.hello)),
  }
})

let wrapper: VueWrapper | null = null
const WorkspaceControls = defineComponent({
  components: { StatusBar, ManualTab, RunBar },
  setup: () => ({ panel }),
  template: '<div><StatusBar/><ManualTab :touch-off="panel.tab === \'probe\'"/><RunBar/></div>',
})
const originalScrollTo = Object.getOwnPropertyDescriptor(HTMLElement.prototype, 'scrollTo')
beforeEach(() => {
  vi.useFakeTimers(); vi.mocked(send).mockClear()
  // jsdom has no scrolling implementation; the MDI history scroll is unrelated to this check.
  Object.defineProperty(HTMLElement.prototype, 'scrollTo', { configurable: true, value: vi.fn() })
  panel.hello = { ...hello, max_jog_velocity: 1500, axis_max_jog_velocity: { x: 1500, y: 1500, z: 750 } }
  panel.status = new SimMachine({ emit: (): void => undefined }).status()
  panel.status.machine = 'on'; panel.status.program.file = '/sim/test.ngc'
  panel.link = 'live'; panel.unit = 'mm'; panel.tab = 'manual'; touchInput.value = false
  panel.workOffsets = null
  panel.preview = null
  panel.startupSpindleRpm = 0;
  panel.jogVelocity = 1500; panel.jogIncrement = 1
})
afterEach(() => {
  wrapper?.unmount(); vi.useRealTimers()
  if (originalScrollTo === undefined) Reflect.deleteProperty(HTMLElement.prototype, 'scrollTo')
  else Object.defineProperty(HTMLElement.prototype, 'scrollTo', originalScrollTo)
})

describe('capability-aware controls', () => {
  it.each([
    [undefined, ['Flood', 'Mist']],
    [{ flood: true, mist: false }, ['Flood']],
    [{ flood: false, mist: true }, ['Mist']],
    [{ flood: false, mist: false }, []],
  ] as const)('offers only configured coolant outputs: %j', (outputs, labels) => {
    if (!panel.hello) throw new Error('Missing hello')
    if (outputs === undefined) delete panel.hello.coolant
    else panel.hello.coolant = outputs
    wrapper = mount(ManualTab)
    expect(wrapper.findAll('.coolant button').map((b) => b.text())).toEqual(labels)
    expect(wrapper.find('.coolant').exists()).toBe(labels.length > 0)
  })
  it('shows power state once until a card action needs to explain it', async () => {
    if (panel.status === null) throw new Error('Missing status')
    panel.status.machine = 'off'
    panel.startupSpindleRpm = 1400
    wrapper = mount(WorkspaceControls, { attachTo: document.body })
    expect(wrapper.get('.message-band .machine-state').text()).toBe('Machine is off')
    expect(wrapper.findAll('.setter-band p').map((band) => band.text())).toEqual([
      'Axis limits: Z 750 mm/min', 'Enter a G-code command.', 'Set the speed before starting the spindle.',
    ])
    await wrapper.get('.jogbtn').trigger('click')
    expect(wrapper.get('.jog .setter-band p').text()).toBe('Machine is off')
    expect(wrapper.get('.mdi .setter-band p').text()).toBe('Enter a G-code command.')
    await wrapper.get('input[aria-label="MDI command"]').setValue('G0 X1')
    wrapper.get<HTMLButtonElement>('.mdi button[type="submit"]').element.click()
    await nextTick()
    expect(wrapper.get('.mdi .setter-band p').text()).toBe('Machine is off')
    await wrapper.get('.utilities button:last-of-type').trigger('click')
    expect(wrapper.get('.utilities .setter-band p').text()).toBe('Machine is off')
    expect(send).not.toHaveBeenCalled()
    panel.status.machine = 'on'
    await nextTick()
    expect(wrapper.find('.message-band .machine-state').exists()).toBe(false)
    expect(wrapper.get('.mdi .setter-band p').text()).toBe('Axes are not homed')
    expect(wrapper.get('.jog .setter-band p').text()).toBe('Axis limits: Z 750 mm/min')
  })
  it('keeps probe-specific guidance in the Touch off card', async () => {
    if (panel.status === null) throw new Error('Missing status')
    panel.status.machine = 'off'
    panel.status.tool.in_spindle = 1
    panel.tab = 'probe'
    panel.hello = { ...hello, touch_probe_tool: 99, fixed_setter: {
      reference_tool: 98, travel_mm: 15, feed_mm_min: 240, xy_tolerance_mm: 0.5, material_travel_mm: 20,
    } }
    wrapper = mount(WorkspaceControls)
    expect(wrapper.get('.machine-state').text()).toBe('Machine is off')
    expect(wrapper.get('.probe-card .setter-band p').text()).toBe('Load T99 to probe material.')
    expect(wrapper.get('.jog .setter-band p').text()).not.toContain('Load T99')
    vi.mocked(send).mockClear()
    await wrapper.get('.probe-actions button').trigger('click')
    expect(wrapper.get('.probe-card .setter-band p').text()).toBe('Machine is off')
    expect(wrapper.get('.jog .setter-band p').text()).not.toContain('Machine is off')
    expect(send).not.toHaveBeenCalled()
  })
  it('shows reported active WCS offsets, converts units and preserves unknown values', async () => {
    if (!panel.status) throw new Error('Missing status')
    panel.status.offsets.wcs = 1
    panel.workOffsets = { type: 'work_offsets', revision: 1, systems: [
      { wcs: 1, offset: [25.4, -50.8, 0], rotation_xy: 0, available: true },
      { wcs: 2, offset: [0, 0, 0], rotation_xy: 0, available: false },
    ] }
    wrapper = mount(ManualTab)
    expect(wrapper.get('.wcs-values').text()).toContain('X 25.400')
    panel.unit = 'inch'; await nextTick()
    expect(wrapper.get('.wcs-values').text()).toContain('Y -2.0000')
    panel.status.offsets.wcs = 2; await nextTick()
    expect(wrapper.get('.wcs-values').text()).toContain('G55')
    expect(wrapper.get('.wcs-values').text()).toContain('X —')
    panel.status.offsets.wcs = 1; panel.link = 'stale'; await nextTick()
    expect(wrapper.get('.wcs-values').text()).toContain('Last reported')
  })
  it('has explicit override reset buttons and concise Stop text', async () => {
    if (!panel.status) throw new Error('Missing status')
    panel.status.motion.feed_override = 0.45
    wrapper = mount(RunBar)
    expect(wrapper.get('.stop-button').text()).toBe('Stop')
    expect(wrapper.get('.stop-button').attributes('aria-label')).toBe('Stop program, MDI, jog and spindle')
    expect(wrapper.findAll('.override .value').every((value) => value.element.tagName === 'SPAN')).toBe(true)
    await wrapper.get('button[aria-label="Reset feed override to 100%"] ').trigger('click')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'set_override', which: 'feed', value: 1 })
    vi.mocked(send).mockClear(); panel.link = 'stale'; await nextTick()
    await wrapper.get('button[aria-label="Reset feed override to 100%"] ').trigger('click')
    expect(send).not.toHaveBeenCalled()
  })
  it('puts probing in its own workspace with jogging, WCS and Stop', async () => {
    wrapper = mount(WorkspaceControls)
    expect(wrapper.find('.em-nav').text()).not.toContain('Touch off')
    panel.hello = { ...hello, touch_probe_tool: 99, fixed_setter: {
      reference_tool: 98, travel_mm: 15, feed_mm_min: 240, xy_tolerance_mm: 0.5, material_travel_mm: 20,
    } }
    await nextTick()
    expect(wrapper.find('.material-probe').exists()).toBe(false)
    expect(wrapper.find('.setter-controls').exists()).toBe(false)
    const nav = wrapper.get('.em-nav').findAll('button')
    expect(nav.map((button) => button.text())).toEqual(['Manual', 'Machine', 'Tools', 'Touch off'])
    await nav[3]?.trigger('click')
    expect(wrapper.get('.em-nav [aria-current]').text()).toBe('Touch off')
    expect(wrapper.findAll('.material-probe button').map((button) => button.text())).toEqual(['Z top', 'X−', 'X+', 'Y−', 'Y+'])
    expect(wrapper.find('.wcs-values').exists()).toBe(true)
    expect(wrapper.text()).toContain('Stop jog')
    expect(wrapper.get('.stop-button').text()).toBe('Stop')
    expect(wrapper.find('.mdi').exists()).toBe(false)
    expect(wrapper.text()).not.toMatch(/Spindle override|Cycle start/)
  })
  it('has no native hover titles on touch controls, including changing blocked reasons', async () => {
    wrapper = mount(WorkspaceControls)
    expect(wrapper.find('[title]').exists()).toBe(true)
    touchInput.value = true; await nextTick()
    expect(wrapper.find('[title]').exists()).toBe(false)
    if (panel.status) panel.status.machine = 'off'
    await nextTick()
    expect(wrapper.find('[title]').exists()).toBe(false)
    expect(wrapper.find('[aria-description]').exists()).toBe(true)
  })
  it('uses exact inch presets and requires a new step choice after changing units', async () => {
    wrapper = mount(ManualTab)
    panel.unit = 'inch'; await nextTick()
    expect(wrapper.text()).toContain('Choose a step in inches')
    window.dispatchEvent(new KeyboardEvent('keydown', { code: 'ArrowRight' }))
    window.dispatchEvent(new KeyboardEvent('keyup', { code: 'ArrowRight' }))
    expect(send).not.toHaveBeenCalled()
    const step = wrapper.findAll('.increments button').find((button) => button.text() === '0.001')
    expect(step).toBeDefined(); await step?.trigger('click')
    window.dispatchEvent(new KeyboardEvent('keydown', { code: 'ArrowRight' }))
    window.dispatchEvent(new KeyboardEvent('keyup', { code: 'ArrowRight' }))
    expect(send).toHaveBeenCalledWith({ type: 'jog_step', axis: 'x', distance: 0.0254, velocity: 1500 })
    vi.mocked(send).mockClear(); panel.unit = 'mm'; await nextTick()
    expect(wrapper.text()).toContain('Choose a step in millimeters')
    window.dispatchEvent(new KeyboardEvent('keydown', { code: 'ArrowRight' }))
    expect(send).not.toHaveBeenCalled()
  })

  it('offers direct power-on when already reset, without a redundant reset action', async () => {
    if (panel.status === null) throw new Error('Missing status')
    panel.status.machine = 'off'
    wrapper = mount(StatusBar)
    const reset = wrapper.findAll('button').find((button) => button.text() === 'Reset E-stop')
    const on = wrapper.findAll('button').find((button) => button.text() === 'Machine on')
    expect(reset).toBeUndefined(); expect(on).toBeDefined()
    expect(on?.element.closest('details')).toBeNull()
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
    const actions = wrapper.findAll('.power-control')
    expect(actions).toHaveLength(1)
    for (const button of actions) {
      expect(button.attributes('disabled')).toBeDefined()
      await button.trigger('click')
    }
    expect(send).not.toHaveBeenCalled()
    await wrapper.get('.estop').trigger('click')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'set_machine', state: 'estop' })
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

  it('shows and sends a fractional startup jog speed without rounding it to a different value', () => {
    panel.jogVelocity = 3.5
    wrapper = mount(ManualTab)
    expect(wrapper.get('.speed').text()).toBe('3.5')
    expect(wrapper.get('input[aria-label="Jog speed"]').attributes('aria-valuetext')).toBe('3.5 mm/min')
    window.dispatchEvent(new KeyboardEvent('keydown', { code: 'ArrowRight' }))
    window.dispatchEvent(new KeyboardEvent('keyup', { code: 'ArrowRight' }))
    expect(send).toHaveBeenCalledWith({ type: 'jog_step', axis: 'x', distance: 1, velocity: 3.5 })
  })

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
    expect(start().attributes('title')).toBe('Homing in progress')
  })

  it('shows a blocking program check without issuing a Run request', async () => {
    if (!panel.status) throw new Error('Missing status')
    panel.hello = { ...hello, homing_required: false, max_spindle_rpm: 1500 }
    panel.status.preview_context = 9
    panel.preview = { type: 'preview', context: 9, state: 'ready', segments: [], bounds: null, dwells: [], error: null,
      check: { tools: [{ tool: 98, length: 'missing' }], frames: [], feed: null, spindle: null,
        units: ['mm'], travel: 'fits', errors: ['T98 has no verified length.'], warnings: [] } }
    wrapper = mount(RunBar, { global: { stubs: { Teleport: true } } })
    await wrapper.get('.main-action').trigger('click')
    expect(wrapper.get('[role="dialog"]').text()).toContain('Cycle start blocked')
    expect(wrapper.get('[role="dialog"]').text()).toContain('T98 has no verified length.')
    expect(wrapper.findAll('[role="dialog"] button').some((button) => button.text().includes('Run program'))).toBe(false)
    expect(send).not.toHaveBeenCalled()
  })

  it('starts directly when physical travel is the only unverified item', async () => {
    if (!panel.status) throw new Error('Missing status')
    panel.hello = { ...hello, homing_required: false }
    panel.status.preview_context = 9
    panel.preview = { type: 'preview', context: 9, state: 'ready', segments: [], bounds: null, dwells: [], error: null,
      check: { tools: [], frames: [], feed: null, spindle: null,
        units: ['mm'], travel: 'unknown', errors: [], warnings: [] } }
    wrapper = mount(RunBar, { global: { stubs: { Teleport: true } } })
    await wrapper.get('.main-action').trigger('click')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'run', preview_context: 9, display_units: 'mm' })
    expect(wrapper.find('[role="dialog"]').exists()).toBe(false)
  })

  it('confirms warnings against the reviewed context and rejects a stale dialog', async () => {
    if (!panel.status) throw new Error('Missing status')
    panel.hello = { ...hello, homing_required: false }
    panel.status.preview_context = 9
    panel.preview = { type: 'preview', context: 9, state: 'ready', segments: [], bounds: null, dwells: [], error: null,
      check: { tools: [], frames: [], feed: null, spindle: null,
        units: ['inch'], travel: 'unknown', errors: [], warnings: [] } }
    wrapper = mount(RunBar, { global: { stubs: { Teleport: true } } })
    await wrapper.get('.main-action').trigger('click')
    expect(wrapper.get('[role="dialog"]').text()).toContain('Program units differ')
    expect(send).not.toHaveBeenCalled()
    await wrapper.get('[role="dialog"] button.primary').trigger('click')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'run', preview_context: 9,
      display_units: 'mm', confirmed: true })
    vi.mocked(send).mockClear()
    await wrapper.get('.main-action').trigger('click')
    panel.status.preview_context = 10
    await nextTick()
    expect(wrapper.get('[role="dialog"]').text()).toContain('The program check changed')
    expect(wrapper.find('[role="dialog"] button.primary').exists()).toBe(false)
    expect(send).not.toHaveBeenCalled()
  })
})
