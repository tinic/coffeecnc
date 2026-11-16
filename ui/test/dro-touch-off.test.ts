import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { defineComponent, nextTick } from 'vue'
import DroPanel from '../src/components/DroPanel.vue'
import NumericEntryDialog from '../src/components/NumericEntryDialog.vue'
import NumberDialog from '../src/components/NumberDialog.vue'
import { mdiBlock, panel, send } from '../src/machine'
import { touchInput } from '../src/numeric-input'
import { SimMachine } from '../src/sim/machine'

vi.mock('../src/machine', async () => {
  const { computed, reactive, ref } = await import('vue')
  const panel = reactive({ hello: null, unit: 'mm', tab: 'manual', status: null, link: 'live' }) as typeof import('../src/machine').panel
  return { panel, reported: computed(() => panel.status), axes: computed(() => panel.status?.axes ?? []),
    live: computed(() => panel.link === 'live'), mdiBlock: ref(null), send: vi.fn() }
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
  it.each(['.number', '.letter', '.touch'])('opens from %s with the work coordinate and does not send until confirmation', async (target) => {
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
    else { await wrapper.get('.basis').trigger('click'); if (mode === 'distance') await wrapper.get('.basis').trigger('click') }
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
    else await wrapper.get('.basis').trigger('click')
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
