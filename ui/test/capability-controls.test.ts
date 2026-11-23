import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { defineComponent, nextTick } from 'vue'
import ManualTab from '../src/components/ManualTab.vue'
import RunBar from '../src/components/RunBar.vue'
import StatusBar from '../src/components/StatusBar.vue'
import { panel, send } from '../src/machine'
import { touchInput } from '../src/numeric-input'
import { breakpointSelection } from '../src/program-breakpoint'
import { hello, SimMachine } from '../src/sim/machine'

// The guide's WebGL chunk must not load in the background (#279).
vi.mock('../src/components/ProbeGuideCanvas.vue', () => import('./canvas-stub'))
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
  panel.program = null
  panel.preview = null
  panel.startupSpindleRpm = 0;
  panel.jogVelocity = 1500; panel.jogIncrement = 1
})
afterEach(() => {
  wrapper?.unmount(); vi.useRealTimers()
  breakpointSelection.value = null
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
  it('marks an axis that stands at its reference mark and homes only axes whose mark is known', async () => {
    if (panel.status === null) throw new Error('Missing status')
    wrapper = mount(ManualTab)
    expect(wrapper.findAll('.homeaxis.marked')).toHaveLength(0)
    panel.status.axes = panel.status.axes.map((axis) => ({ ...axis, homed: false, reference_mark: axis.axis !== 'z', at_reference_mark: axis.axis === 'y' }))
    await nextTick()
    const buttons = wrapper.findAll('.homeaxis')
    expect(buttons.map((button) => button.classes('marked'))).toEqual([false, true, false])
    expect(buttons.map((button) => button.get('.homestate').text())).toEqual(['not homed', 'at mark', 'find mark'])
    expect(buttons.map((button) => button.attributes('aria-disabled'))).toEqual(['false', 'false', 'true'])
    const [x, , z] = buttons
    if (x === undefined || z === undefined) throw new Error('Missing home buttons')
    await z.trigger('click')
    await wrapper.get('.homes > .btn').trigger('click')
    expect(send).not.toHaveBeenCalled()
    expect(wrapper.get('.jog .setter-band p').text()).toBe('Move Z across its reference mark first')
    await x.trigger('click')
    expect(send).toHaveBeenCalledWith({ type: 'home', axis: 'x' })
    panel.status.machine = 'off'
    await nextTick()
    expect(wrapper.findAll('.homeaxis.marked')).toHaveLength(1)
    panel.status.axes = panel.status.axes.map((axis) => ({ ...axis, at_reference_mark: axis.axis !== 'y' }))
    await nextTick()
    expect(wrapper.findAll('.homeaxis').map((button) => button.classes('marked'))).toEqual([true, false, true])
    panel.status.axes = panel.status.axes.map((axis) => ({ ...axis, homed: true }))
    await nextTick()
    expect(wrapper.findAll('.homeaxis.marked')).toHaveLength(0)
  })
  it('shows power state once until a card action needs to explain it', async () => {
    if (panel.status === null) throw new Error('Missing status')
    panel.status.machine = 'off'
    panel.startupSpindleRpm = 1400
    wrapper = mount(WorkspaceControls, { attachTo: document.body })
    expect(wrapper.get('.message-band .machine-state').text()).toBe('Machine is off')
    expect(wrapper.findAll('.setter-band p').map((band) => band.text())).toEqual([
      'Axis limits: Z 750 mm/min',
    ])
    await wrapper.get('.jogbtn').trigger('click')
    expect(wrapper.get('.jog .setter-band p').text()).toBe('Machine is off')
    expect(wrapper.get('.mdi .setter-band').text()).toBe('')
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
  it('shows a spindle refusal only on press and clears it once ready', async () => {
    wrapper = mount(ManualTab)
    expect(wrapper.get('.spindle-status.info').text()).toBe('')
    const forward = wrapper.findAll('button').find(button => button.text() === 'Forward')
    if (!forward) throw new Error('Missing Forward button')
    await forward.trigger('click')
    expect(wrapper.get('.spindle-status.blocked p').text()).toBe('Enter a spindle speed above zero')
    expect(send).not.toHaveBeenCalled()
    await wrapper.get('input[aria-label="Spindle speed, rpm"]').setValue('1200')
    expect(wrapper.get('.spindle-status.info').text()).toBe('')
  })
  it('keeps probe-specific guidance in the Touch off card', async () => {
    if (panel.status === null) throw new Error('Missing status')
    panel.status.machine = 'off'
    panel.status.tool.in_spindle = 1
    panel.tab = 'probe'
    panel.hello = { ...hello, touch_probe_tool: 99, fixed_setter: {
      reference_tool: 98, travel_mm: 15, feed_mm_min: 240, xy_tolerance_mm: 0.5, material_travel_mm: 20,
    } }
    vi.stubGlobal('matchMedia', () => ({ matches: true }))
    wrapper = mount(WorkspaceControls, { global: { stubs: { Teleport: true, GuideCanvas: true } } })
    expect(wrapper.get('.machine-state').text()).toBe('Machine is off')
    expect(wrapper.get('.probe-card .setter-band p').text()).toBe('Load T99 to probe material.')
    expect(wrapper.get('.jog .setter-band').text()).not.toContain('Load T99')
    vi.mocked(send).mockClear()
    await wrapper.get('.probe-actions button').trigger('click')
    await wrapper.get('.start-probe').trigger('click')
    vi.unstubAllGlobals()
    expect(wrapper.get('.probe-card .setter-band p').text()).toBe('Machine is off')
    expect(wrapper.get('.jog .setter-band').text()).not.toContain('Machine is off')
    expect(send).not.toHaveBeenCalled()
  })
  it('leaves AUTO progress to the Machine footer and preserves paused Z instructions', async () => {
    const s = panel.status
    if (!s?.program.file) throw new Error('Missing program')
    panel.tab = 'program'
    s.mode = 'auto'
    s.interp = 'reading'
    wrapper = mount(StatusBar)
    expect(wrapper.get('.machine-state').text()).toBe('Program running')
    panel.program = { type: 'program', file: s.program.file, revision: s.program.revision, lines: ['M30'], truncated: false, stamp: '0' }
    await nextTick()
    expect(wrapper.find('.message-band .machine-state').exists()).toBe(false)
    expect(wrapper.get('.state-pill').text()).toBe('Program running')
    s.interp = 'paused'
    s.wait = { kind: 'paused' }
    s.paused_z = { offset: 0, max_lift: 10, displaced: false, reason: 'Stop the spindle before jogging Z.', spindle: 'can_stop' }
    await nextTick()
    expect(wrapper.get('.machine-state').text()).toBe('Stop the spindle before jogging Z.')
    panel.tab = 'manual'
    await nextTick()
    expect(wrapper.get('.machine-state').text()).toBe('Program paused')
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
  it('shows a bounded Z step in Machine during an eligible AUTO pause', async () => {
    if (!panel.status) throw new Error('Missing status')
    panel.tab = 'program'
    panel.status.mode = 'auto'
    panel.status.interp = 'paused'
    panel.status.wait = { kind: 'paused' }
    panel.status.paused_z = { offset: 0, max_lift: 10, displaced: false, reason: 'Home all axes' }
    wrapper = mount(RunBar)
    const lift = wrapper.get('button[aria-label="Lift paused Z"]')
    expect(lift.attributes('disabled')).toBeDefined()
    panel.status.paused_z.reason = ''
    panel.unit = 'inch'
    await nextTick()
    expect(wrapper.get('button[aria-label="Lower paused Z"]').attributes('disabled')).toBeDefined()
    await lift.trigger('click')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'jog_paused_z', distance: 1.27 })
    expect(lift.attributes('disabled')).toBeDefined()
    expect(wrapper.get('.main-action').attributes('disabled')).toBeDefined()
  })
  it('requires a fresh confirmation before stopping a running spindle to lift Z', async () => {
    if (!panel.status) throw new Error('Missing status')
    panel.tab = 'program'
    panel.status.mode = 'auto'
    panel.status.interp = 'paused'
    panel.status.wait = { kind: 'paused' }
    panel.status.spindle.direction = 'forward'
    panel.status.spindle.commanded_rpm = 650
    panel.status.paused_z = { offset: 0, max_lift: 10, displaced: false, reason: 'Stop spindle', spindle: 'can_stop' }
    wrapper = mount(RunBar, { global: { stubs: { Teleport: true } } })
    const lift = wrapper.get('button[aria-label="Lift paused Z"]')
    await lift.trigger('click')
    expect(wrapper.get('[role="dialog"]').text()).toContain('Stop the spindle to lift Z?')
    expect(send).not.toHaveBeenCalled()
    expect(wrapper.get('.main-action').attributes('disabled')).toBeDefined()
    expect(wrapper.get('.stop-button').attributes('disabled')).toBeUndefined()
    panel.status.program.current_line += 1
    await nextTick()
    expect(wrapper.find('[role="dialog"]').exists()).toBe(false)
    await lift.trigger('click')
    const buttons = wrapper.get('[role="dialog"]').findAll('button')
    const confirm = buttons.find((button) => button.text() === 'Stop spindle & lift')
    if (!confirm) throw new Error('Missing spindle confirmation')
    await confirm.trigger('click')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'jog_paused_z', distance: 1, stop_spindle: true })
  })
  it('labels the verified return and explains an offset left after Stop', async () => {
    if (!panel.status) throw new Error('Missing status')
    panel.tab = 'program'
    panel.status.mode = 'auto'
    panel.status.interp = 'paused'
    panel.status.wait = { kind: 'paused' }
    panel.status.paused_z = { offset: 2, max_lift: 10, displaced: true, reason: '' }
    wrapper = mount(RunBar)
    expect(wrapper.get('.paused-z-reading').text()).toContain('2.000 mm')
    expect(wrapper.get('.main-action').text()).toBe('Return Z & resume')
    await wrapper.get('.main-action').trigger('click')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'resume' })
    expect(wrapper.get('.stop-button').attributes('disabled')).toBeUndefined()
    panel.status.interp = 'idle'
    panel.status.wait = { kind: 'none' }
    await nextTick()
    expect(wrapper.find('.paused-z').exists()).toBe(false)
    expect(wrapper.get('.main-action').attributes('disabled')).toBeDefined()
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
  it('follows a drag without asking, and asks once on release', async () => {
    // turo: the slider jumped while dragging; apply only when releasing.
    if (!panel.status) throw new Error('Missing status')
    panel.status.motion.feed_override = 1
    wrapper = mount(RunBar)
    const slider = wrapper.get<HTMLInputElement>('input[aria-label="Feed override"]')
    // setValue would also fire change; a drag fires only input until release.
    for (const value of ['110', '130', '150']) { slider.element.value = value; await slider.trigger('input') }
    expect(send).not.toHaveBeenCalled()
    expect(slider.element.value).toBe('150')
    expect(wrapper.get('.override .value').text()).toBe('150 %')
    await slider.trigger('change')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'set_override', which: 'feed', value: 1.5 })
  })
  it('puts maximum velocity in the execution menu and shows the applied cap only below 100%', async () => {
    if (!panel.status) throw new Error('Missing status')
    delete panel.status.motion.velocity_limit
    wrapper = mount(RunBar)
    const selector = 'input[aria-label="Maximum velocity"]'
    expect(wrapper.find(selector).exists()).toBe(false)
    panel.status.motion.velocity_limit = 1
    await nextTick()
    expect(wrapper.find('.active-options').exists()).toBe(false)
    await wrapper.get('.popover').get(selector).setValue('40')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'set_override', which: 'velocity_limit', value: 0.4 })
    expect(wrapper.find('.active-options').exists()).toBe(false)
    panel.status.motion.velocity_limit = 0.4
    await nextTick()
    expect(wrapper.get('.more-button').attributes('aria-label')).toBe('More execution controls · Maximum velocity 40 %')
    expect(wrapper.get('.active-options .velocity').text()).toBe('V40')
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
    expect(wrapper.findAll('.material-probe button').map((button) => button.text())).toEqual(['Z top', 'X−', 'X+', 'Y−', 'Y+', 'Features…', 'Calibrate ball…', 'Measure only'])
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

  it('binds the selected source line to the reviewed run and displays Resume at the pause', async () => {
    if (!panel.status?.program.file) throw new Error('Missing loaded program')
    panel.hello = { ...hello, homing_required: false }
    panel.status.preview_context = 9
    panel.preview = { type: 'preview', context: 9, state: 'ready', bounds: null, dwells: [], error: null,
      segments: [{ line: 5, kind: 'feed', tool: 1, feed: 900, points: [0, 0, 20, 0, 0, 2] }],
      check: { tools: [], frames: [], feed: null, spindle: null,
        units: ['mm'], travel: 'unknown', errors: [], warnings: [] } }
    wrapper = mount(RunBar)
    breakpointSelection.value = { file: panel.status.program.file, revision: panel.status.program.revision, line: 5, text: 'G1 Z2 F900' }
    await nextTick()
    expect(wrapper.get('.main-action').text()).toContain('Run to line 5')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'run', preview_context: 9,
      display_units: 'mm', breakpoint: { line: 5, text: 'G1 Z2 F900' } })
    panel.status.mode = 'auto'
    panel.status.interp = 'paused'
    panel.status.wait = { kind: 'paused' }
    panel.status.program.breakpoint = { line: 5, reached: true }
    await nextTick()
    expect(breakpointSelection.value).toBeNull()
    expect(wrapper.get('.main-action').text()).toContain('Resume')
  })

  it('keeps Run and Run to line on the path of a preview that ended early, after the review', async () => {
    if (!panel.status?.program.file) throw new Error('Missing loaded program')
    panel.hello = { ...hello, homing_required: false }
    panel.status.preview_context = 9
    const partial = 'CoffeeCNC could only check part of this program.'
    panel.preview = { type: 'preview', context: 9, state: 'failed', bounds: null, dwells: [],
      error: { line: 6, text: 'The preview ends at line 6: M6 is remapped on this machine ([RS274NGC] REMAP): what it does is not known to the preview.' },
      segments: [{ line: 5, kind: 'feed', tool: 1, feed: 900, points: [0, 0, 20, 0, 0, 2] }],
      check: { tools: [], frames: [], feed: null, spindle: null,
        units: ['mm'], travel: 'unknown', errors: [], warnings: [partial] } }
    wrapper = mount(RunBar, { global: { stubs: { Teleport: true } } })
    const action = (): ReturnType<VueWrapper['get']> => wrapper?.get('.main-action') ?? ((): never => { throw new Error('Missing RunBar') })()
    // Run: offered, with the partial check to review first.
    expect(action().attributes('disabled')).toBeUndefined()
    await action().trigger('click')
    expect(wrapper.get('[role="dialog"]').text()).toContain(partial)
    expect(send).not.toHaveBeenCalled()
    await wrapper.get('[role="dialog"] button.primary').trigger('click')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'run', preview_context: 9,
      display_units: 'mm', confirmed: true })
    vi.mocked(send).mockClear()

    // Run to a line after the end of the preview: refused, with the reason.
    breakpointSelection.value = { file: panel.status.program.file, revision: panel.status.program.revision, line: 7, text: 'G1 X5' }
    await nextTick()
    expect(action().attributes('disabled')).toBeDefined()
    expect(action().attributes('aria-description')).toBe('The preview ends before this line: choose a line on the previewed path')
    expect(wrapper.find('[role="dialog"]').exists()).toBe(false)
    // Run to a line on the previewed path: offered, after the same review.
    breakpointSelection.value = { file: panel.status.program.file, revision: panel.status.program.revision, line: 5, text: 'G1 Z2 F900' }
    await nextTick()
    expect(action().text()).toContain('Run to line 5')
    expect(wrapper.get('[role="dialog"]').text()).toContain(partial)
    expect(send).not.toHaveBeenCalled()
    await wrapper.get('[role="dialog"] button.primary').trigger('click')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'run', preview_context: 9, display_units: 'mm',
      confirmed: true, breakpoint: { line: 5, text: 'G1 Z2 F900' } })

    // A preview that was not computed at all has no path: Run to line stays refused.
    vi.mocked(send).mockClear()
    breakpointSelection.value = null
    panel.preview = { type: 'preview', context: 9, state: 'unavailable', bounds: null, dwells: [], segments: [],
      error: { line: 0, text: 'This machine has a random tool changer: no preview is computed.' } }
    await nextTick()
    breakpointSelection.value = { file: panel.status.program.file, revision: panel.status.program.revision, line: 5, text: 'G1 Z2 F900' }
    await nextTick()
    expect(action().attributes('disabled')).toBeDefined()
    expect(action().attributes('aria-description')).toBe('A breakpoint needs a complete program check')
    expect(send).not.toHaveBeenCalled()
  })
})

describe('override sliders', () => {
  function slider(view: VueWrapper, label: string): ReturnType<VueWrapper['get']> {
    return view.get(`input[type="range"][aria-label="${label}"]`)
  }
  it('leave the arrow and page keys to jogging after a touch', () => {
    wrapper = mount(WorkspaceControls, { attachTo: document.body })
    for (const label of ['Feed override', 'Spindle override', 'Jog speed']) {
      vi.mocked(send).mockClear()
      const element = slider(wrapper, label).element as HTMLInputElement
      element.focus()
      expect(document.activeElement).toBe(element)
      const key = new KeyboardEvent('keydown', { code: 'PageUp', bubbles: true, cancelable: true })
      element.dispatchEvent(key)
      window.dispatchEvent(new KeyboardEvent('keyup', { code: 'PageUp' }))
      expect(key.defaultPrevented).toBe(true)
      expect(send).toHaveBeenCalledWith({ type: 'jog_step', axis: 'z', distance: 1, velocity: 750 })
      expect(vi.mocked(send).mock.calls.some(([command]) => command.type === 'set_override')).toBe(false)
    }
    const element = wrapper.element as Element
    wrapper.unmount(); element.remove(); wrapper = null
  })
  it('show the machine again after a drag back to the start, a cancelled drag or Reset', async () => {
    wrapper = mount(RunBar)
    const view = wrapper
    const readout = (): string => view.findAll('.override').find((row) => row.text().includes('Feed override'))?.get('.value').text() ?? ''
    const feed = slider(view, 'Feed override')
    /** The finger moves the slider: an input event, no change event yet. */
    const drag = async (value: string): Promise<void> => {
      ;(feed.element as HTMLInputElement).value = value
      await feed.trigger('input')
    }
    const machine = async (factor: number): Promise<void> => {
      if (panel.status === null) throw new Error('Missing status')
      panel.status = { ...panel.status, motion: { ...panel.status.motion, feed_override: factor } }
      await nextTick()
    }
    await machine(1)
    // Away and back to 100 %: a browser fires no change event for that release.
    await drag('80'); await drag('100'); await feed.trigger('pointerup')
    await machine(0.5)
    expect(readout()).toBe('50 %')
    expect(send).not.toHaveBeenCalled()
    // A cancelled drag asks for nothing.
    await drag('70'); await feed.trigger('pointercancel')
    await machine(0.6)
    expect(readout()).toBe('60 %')
    expect(send).not.toHaveBeenCalled()
    // Reset during a drag shows the machine again.
    await drag('70')
    expect(readout()).toBe('70 %')
    await view.findAll('button').find((button) => button.text() === 'Reset 100%')?.trigger('click')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'set_override', which: 'feed', value: 1 })
    await machine(1)
    expect(readout()).toBe('100 %')
    // A release asks once for the value shown.
    vi.mocked(send).mockClear()
    await drag('90'); await feed.trigger('pointerup'); await feed.trigger('change')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'set_override', which: 'feed', value: 0.9 })
  })
})
