import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { defineComponent, nextTick } from 'vue'
import DroPanel from '../src/components/DroPanel.vue'
import NumericEntryDialog from '../src/components/NumericEntryDialog.vue'
import NumberDialog from '../src/components/NumberDialog.vue'
import CoordinateConfirm from '../src/components/CoordinateConfirm.vue'
import { mdiBlock, panel, send } from '../src/machine'
import { touchInput } from '../src/numeric-input'
import { SimMachine } from '../src/sim/machine'

vi.mock('../src/machine', async () => {
  const { computed, reactive, ref } = await import('vue')
  const panel = reactive({ hello: null, unit: 'mm', tab: 'manual', status: null, link: 'live' }) as typeof import('../src/machine').panel
  return { panel, reported: computed(() => panel.status), axes: computed(() => panel.status?.axes ?? []),
    rotaryAxes: computed(() => panel.status?.rotary_axes ?? []), live: computed(() => panel.link === 'live'), mdiBlock: ref(null), send: vi.fn() }
})
const fixture = defineComponent({
  components: { DroPanel, NumericEntryDialog },
  template: '<div><DroPanel /><div id="dialogs"></div><div id="workspace-dialogs"></div><NumericEntryDialog /></div>',
})
let wrapper: VueWrapper
const field = (): HTMLInputElement => wrapper.get('[role="dialog"] input').element as HTMLInputElement
async function press(label: string): Promise<void> {
  const button = wrapper.findAll('[role="dialog"] button').find((candidate) => candidate.text() === label)
  if (!button) throw new Error(`Missing ${label}`)
  await button.trigger('click')
}
beforeEach(() => {
  panel.status = new SimMachine({ emit: (): void => undefined }).status()
  panel.status.offsets.wcs = 1
  panel.status.axes.forEach((axis) => { axis.work = axis.axis === 'x' ? 12.345678 : -25.4 })
  panel.unit = 'mm'; panel.tab = 'manual'; panel.hello = null; panel.link = 'live'
  Object.assign(mdiBlock, { value: null })
  touchInput.value = false
  vi.mocked(send).mockClear()
  wrapper = mount(fixture, { attachTo: document.body })
})
afterEach(() => { const element = wrapper.element as HTMLElement; wrapper.unmount(); element.remove() })

describe('touching the coordinate readout', () => {
  it('cycles a single view button through work, machine and distance without sending a command', async () => {
    const axis = panel.status?.axes[0]
    if (!axis) throw new Error('Missing axis')
    axis.machine = 40; axis.dtg = 2
    await nextTick()
    const view = wrapper.get('.basis-toggle')
    expect(wrapper.findAll('.basis button')).toHaveLength(1)
    expect(view.text()).toBe('Work · G54')
    expect(wrapper.get('.axis .digits').text()).toBe('12.346')
    await view.trigger('click')
    expect(view.text()).toBe('Machine')
    expect(wrapper.get('.axis .digits').text()).toBe('40.000')
    expect(wrapper.get('button[aria-label="Zero X"]').attributes('disabled')).toBeDefined()
    await view.trigger('click')
    expect(view.text()).toBe('To go')
    expect(wrapper.get('.axis .digits').text()).toBe('2.000')
    await view.trigger('click')
    expect(view.text()).toBe('Work · G54')
    expect(wrapper.get('.axis').attributes('disabled')).toBeUndefined()
    if (!panel.status) throw new Error('Missing status')
    panel.status.offsets.wcs = 2
    await nextTick()
    expect(view.text()).toBe('Work · G55')
    expect(view.attributes('aria-label')).toContain('Switch to Machine')
    expect(send).not.toHaveBeenCalled()
  })

  it('pads the readout across signs and magnitudes while keeping the real zero and precision', async () => {
    const axis = panel.status?.axes[0]
    if (!axis) throw new Error('Missing axis')
    const number = wrapper.get('.axis .number')
    const leading = number.get('.leading-zeros')
    expect(leading.attributes('aria-hidden')).toBe('true')
    for (const [position, zeros, display] of [
      [-0.0004, '000', '0000.000'],
      [-12.345678, '00', '-0012.346'],
      [9999.999, '', '9999.999'],
      [10000, '', '10000.000'],
    ] as const) {
      axis.work = position
      await nextTick()
      expect(leading.text()).toBe(zeros)
      expect(number.text()).toBe(display)
    }
    panel.unit = 'inch'
    axis.work = 25.4
    await nextTick()
    expect(leading.text()).toBe('00')
    expect(number.text()).toBe('001.0000')
    expect(number.get('.digits').text()).toBe('1.0000')
    await number.trigger('click')
    expect(field().value).toBe('1.0000')
    expect(send).not.toHaveBeenCalled()
  })

  it('shows only configured rotary axes in degrees in every coordinate basis', async () => {
    expect(wrapper.find('.rotary').exists()).toBe(false)
    if (!panel.status) throw new Error('Missing status')
    panel.status.rotary_axes = [
      { axis: 'a', homed: true, homing: false, work: 32.5, machine: 90, dtg: -2, on_hard_limit: false },
      { axis: 'c', homed: false, homing: true, work: null, machine: null, dtg: null, on_hard_limit: false },
    ]
    await nextTick()
    expect(wrapper.findAll('.rotary-letter').map((label) => label.text())).toEqual(['A', 'C'])
    expect(wrapper.findAll('.rotary-angle').map((value) => value.text())).toEqual(['32.500°', '—'])
    expect(wrapper.get('.rotary-marker').text()).toBe('Homing')
    expect(wrapper.find('.rotary button').exists()).toBe(false)
    await wrapper.get('.units button:last-child').trigger('click')
    expect(wrapper.get('.rotary-angle').text()).toBe('32.500°')
    await wrapper.get('.basis-toggle').trigger('click')
    expect(wrapper.get('.rotary-angle').text()).toBe('90.000°')
    await wrapper.get('.basis-toggle').trigger('click')
    expect(wrapper.get('.rotary-angle').text()).toBe('-2.000°')
    expect(send).not.toHaveBeenCalled()
  })

  it('shows the loaded tool and reported active length compensation in display units', async () => {
    if (!panel.status) throw new Error('Missing status')
    panel.status.tool.in_spindle = 7
    panel.status.active_codes = ['G0', 'G43', 'G54']
    panel.status.offsets.tool[2] = 25.4
    await nextTick()
    expect(wrapper.get('.active-tool').text()).toContain('T7 · G43')
    expect(wrapper.get('.tool-length').text()).toContain('25.400 mm')
    panel.unit = 'inch'; await nextTick()
    expect(wrapper.get('.tool-length').text()).toContain('1.0000 in')
    panel.status.active_codes = ['G49']; panel.status.offsets.tool[2] = 0
    await nextTick()
    expect(wrapper.get('.active-tool').text()).toContain('G49')
    expect(wrapper.get('.tool-length').text()).toContain('0.0000 in')
    panel.link = 'stale'; await nextTick()
    expect(wrapper.get('.active-tool').text()).toContain('Last reported tool')
    // turo: the tool stays on the right of the coordinates on every tab.
    for (const tab of ['program', 'tools'] as const) {
      panel.tab = tab; await nextTick()
      expect(wrapper.find('.active-tool').exists()).toBe(true)
    }
  })
  it.each(['mm', 'inch'] as const)('confirms mixed-unit formulas as millimeters from a %s readout', async (unit) => {
    panel.unit = unit; await nextTick()
    for (const [text, expected] of [['1/4in', 6.35], ['7mm/2', 3.5], ['7/2mm', 3.5]] as const) {
      await wrapper.get('.axis').trigger('click')
      await wrapper.get('[role="dialog"] input').setValue(text)
      expect(send).not.toHaveBeenCalled()
      await press('Set G54 X')
      const command = vi.mocked(send).mock.calls[0]?.[0]
      expect(command?.type).toBe('touch_off')
      if (command?.type !== 'touch_off') throw new Error('Expected touch-off command')
      expect(command.axis).toBe('x')
      expect(command.value).toBeCloseTo(expected, 12)
      vi.mocked(send).mockClear()
    }
  })
  it.each(['.number', '.letter'])('opens from %s with the work coordinate and does not send until confirmation', async (target) => {
    await wrapper.get(`.axis ${target}`).trigger('click')
    expect(field().value).toBe('12.346')
    expect(wrapper.get('[role="dialog"]').attributes('aria-label')).toBe('Touch off X in G54')
    expect(send).not.toHaveBeenCalled()
    await press('Set G54 X')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'touch_off', axis: 'x', value: 12.345678 })
  })

  it('replaces the initial coordinate with the first digit, supports explicit Zero and cancellation', async () => {
    await wrapper.get('.axis .number').trigger('click')
    await press('7')
    expect(field().value).toBe('7')
    await press('Cancel')
    expect(send).not.toHaveBeenCalled()
    await wrapper.get('.axis').trigger('click')
    await press('Zero'); await press('Set G54 X')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'touch_off', axis: 'x', value: 0 })
  })

  it('explicit Zero works when the current coordinate rounds to zero', async () => {
    if (!panel.status?.axes[0]) throw new Error('Missing axis')
    panel.status.axes[0].work = 0.0004
    await nextTick(); await wrapper.get('.axis').trigger('click')
    expect(field().value).toBe('0.000')
    await press('Zero'); await press('Set G54 X')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'touch_off', axis: 'x', value: 0 })
  })

  it('preserves an entered zero while position feedback updates', async () => {
    await wrapper.get('.axis').trigger('click')
    await press('Zero')
    const input = field()
    if (!panel.status) throw new Error('Missing status')
    panel.status = { ...panel.status, axes: panel.status.axes.map((axis) => ({ ...axis, work: axis.work + 0.0002 })) }
    await nextTick()
    expect(field()).toBe(input)
    expect(field().value).toBe('0')
    await press('Set G54 X')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'touch_off', axis: 'x', value: 0 })
  })

  it('prefills inches and converts an edited target back to millimeters', async () => {
    panel.unit = 'inch'; await nextTick()
    await wrapper.findAll('.axis')[1]?.trigger('click')
    expect(field().value).toBe('-1.0000')
    await press('2'); await press('Set G54 Y')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'touch_off', axis: 'y', value: 50.8 })
  })

  it('starts a fresh draft when another axis is tapped while the dialog is open', async () => {
    await wrapper.get('.axis').trigger('click'); await press('7')
    await wrapper.findAll('.axis')[1]?.trigger('click')
    expect(field().value).toBe('-25.400')
    expect(send).not.toHaveBeenCalled()
    await press('Set G54 Y')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'touch_off', axis: 'y', value: -25.4 })
  })

  it.each(['machine', 'distance', 'blocked', 'program'])('does not edit %s coordinates', async (mode) => {
    if (mode === 'blocked') Object.assign(mdiBlock, { value: 'Machine is off' })
    else if (mode === 'program') panel.tab = 'program'
    else {
      await wrapper.get('.basis-toggle').trigger('click')
      if (mode === 'distance') await wrapper.get('.basis-toggle').trigger('click')
    }
    await nextTick()
    expect(wrapper.get('.axis').attributes('disabled')).toBeDefined()
    await wrapper.get('.axis .number').trigger('click')
    expect(wrapper.find('[role="dialog"]').exists()).toBe(false)
    expect(send).not.toHaveBeenCalled()
  })

  it.each(['units', 'wcs', 'blocked', 'workspace', 'session', 'basis'])('invalidates a draft when its %s changes', async (change) => {
    await wrapper.get('.axis').trigger('click')
    const dialog = wrapper.findComponent(NumberDialog)
    if (change === 'units') panel.unit = 'inch'
    else if (change === 'wcs' && panel.status) panel.status.offsets.wcs = 2
    else if (change === 'blocked') Object.assign(mdiBlock, { value: 'No current controller status' })
    else if (change === 'workspace') panel.tab = 'tools'
    else if (change === 'session') Object.assign(panel, { hello: {} })
    else await wrapper.get('.basis-toggle').trigger('click')
    // Even a confirmation queued before Vue removes the dialog cannot apply the draft.
    dialog.vm.$emit('confirm', 10, '10')
    await nextTick()
    expect(wrapper.find('[role="dialog"]').exists()).toBe(false)
    expect(send).not.toHaveBeenCalled()
  })

  it('ignores an opening touch compatibility click over a keypad control', async () => {
    await wrapper.get('.axis').trigger('pointerdown', { pointerType: 'touch' })
    await wrapper.get('.axis').trigger('pointerup', { pointerType: 'touch' })
    await wrapper.get('.axis .number').trigger('click')
    await wrapper.get('[role="dialog"] button[aria-label="Delete the last digit"]').trigger('click')
    expect(field().value).toBe('12.346')
    expect(send).not.toHaveBeenCalled()
  })
})

describe('quick coordinate actions', () => {
  it.each(['mm', 'inch'] as const)('halves the exact work coordinate in %s without display rounding', async (unit) => {
    panel.unit = unit; await nextTick()
    await wrapper.get('button[aria-label="Half X"]').trigger('click')
    expect(send).not.toHaveBeenCalled()
    expect(wrapper.get('[role="dialog"]').attributes('aria-label')).toBe('Half X in G54?')
    expect(document.activeElement?.textContent).toBe('Cancel')
    await press('Set X to half')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'touch_off', axis: 'x', value: 12.345678 / 2, origin: 'half' })
  })
  it('zeroes only the chosen axis after confirmation and can cancel', async () => {
    await wrapper.get('button[aria-label="Zero Y"]').trigger('click')
    await press('Cancel'); expect(send).not.toHaveBeenCalled()
    await wrapper.get('button[aria-label="Zero Y"]').trigger('click')
    await press('Set Y to zero')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'touch_off', axis: 'y', value: 0, origin: 'zero' })
  })
  it.each(['Zero', 'Half'] as const)('keeps the menu and %s confirmation open through feedback changes', async (operation) => {
    if (!panel.status?.axes[0]) throw new Error('Missing axis')
    const menu = wrapper.get('.axis-actions').element as HTMLDetailsElement
    menu.open = true
    panel.status.axes[0].work += 0.0001
    await nextTick()
    expect(menu.open).toBe(true)
    await wrapper.get(`button[aria-label="${operation} X"]`).trigger('click')
    const dialog = wrapper.get('[role="dialog"]').element
    const focus = document.activeElement
    for (const current of [12.345901, 12.345101, 12.345987]) {
      panel.status = { ...panel.status, axes: panel.status.axes.map((axis) => axis.axis === 'x' ? { ...axis, work: current } : axis) }
      await nextTick()
      expect(wrapper.get('[role="dialog"]').element).toBe(dialog)
      expect(document.activeElement).toBe(focus)
      expect(wrapper.findComponent(CoordinateConfirm).props('current')).toBe(current)
      expect(wrapper.findComponent(CoordinateConfirm).props('target')).toBe(operation === 'Half' ? current / 2 : 0)
      expect(send).not.toHaveBeenCalled()
    }
    await press(`Set X to ${operation.toLowerCase()}`)
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'touch_off', axis: 'x', value: operation === 'Half' ? 12.345987 / 2 : 0, origin: operation === 'Half' ? 'half' : 'zero' })
  })
  it.each(['wcs', 'session', 'units', 'blocked', 'basis', 'missing axis', 'invalid coordinate'])('discards a quick confirmation after %s changes', async (change) => {
    await wrapper.get('button[aria-label="Half X"]').trigger('click')
    const dialog = wrapper.findComponent(CoordinateConfirm)
    if (change === 'wcs' && panel.status) panel.status.offsets.wcs = 2
    else if (change === 'session') Object.assign(panel, { hello: {} })
    else if (change === 'units') panel.unit = 'inch'
    else if (change === 'blocked') Object.assign(mdiBlock, { value: 'Machine busy' })
    else if (change === 'missing axis' && panel.status) panel.status.axes = panel.status.axes.filter((axis) => axis.axis !== 'x')
    else if (change === 'invalid coordinate' && panel.status?.axes[0]) panel.status.axes[0].work = NaN
    else await wrapper.get('.basis-toggle').trigger('click')
    dialog.vm.$emit('confirm'); await nextTick()
    expect(send).not.toHaveBeenCalled()
    expect(wrapper.find('[role="dialog"]').exists()).toBe(false)
  })
})
