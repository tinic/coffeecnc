import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { defineComponent, nextTick } from 'vue'
import ManualTab from '../src/components/ManualTab.vue'
import NumericEntryDialog from '../src/components/NumericEntryDialog.vue'
import { panel, send } from '../src/machine'
import { numericEntry, touchInput } from '../src/numeric-input'
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
  panel.link = 'live'; panel.unit = 'mm'; touchInput.value = false
  panel.jogVelocity = 1500; panel.jogIncrement = 1
})
afterEach(() => {
  const element = wrapper?.element as Element | undefined
  wrapper?.unmount(); element?.remove(); numericEntry.value = null; vi.useRealTimers()
  if (originalScrollTo === undefined) Reflect.deleteProperty(HTMLElement.prototype, 'scrollTo')
  else Object.defineProperty(HTMLElement.prototype, 'scrollTo', originalScrollTo)
})

describe('explicit spindle speed confirmation', () => {
  function setup(direction: 'forward' | 'reverse' | 'stopped' = 'forward'): NonNullable<typeof wrapper> {
    if (panel.status === null) throw new Error('Missing status')
    panel.status.spindle.direction = direction
    panel.status.spindle.commanded_rpm = 1200
    wrapper = mount(defineComponent({
      components: { ManualTab, NumericEntryDialog },
      template: '<div><ManualTab/><div id="dialogs"></div><div id="workspace-dialogs"></div><NumericEntryDialog/></div>',
    }), { attachTo: document.body })
    return wrapper
  }
  const selector = 'input[aria-label="Spindle speed, rpm"]'

  it('evaluates an RPM formula only when Enter confirms it', async () => {
    const view = setup('reverse')
    await view.get(selector).setValue('2400/2+300')
    expect(send).not.toHaveBeenCalled()
    await view.get(selector).trigger('keydown', { key: 'Enter' })
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'set_spindle', direction: 'reverse', rpm: 1500 })
  })

  it.each(['forward', 'reverse'] as const)('applies %s RPM once on Enter, never on typing or change', async (direction) => {
    const view = setup(direction)
    const input = view.get<HTMLInputElement>(selector)
    expect(input.element.value).toBe('1200')
    await input.setValue('1800')
    expect(send).not.toHaveBeenCalled()
    await input.trigger('keydown', { key: 'Enter' })
    await input.trigger('keydown', { key: 'Enter', repeat: true })
    await input.trigger('keydown', { key: 'Enter' })
    await input.trigger('blur')
    expect(vi.mocked(send).mock.calls).toEqual([[{ type: 'set_spindle', direction, rpm: 1800 }]])
  })

  it.each(['blur', 'Escape'])('discards an unconfirmed edit on %s and restores commanded RPM', async (event) => {
    const view = setup()
    const input = view.get<HTMLInputElement>(selector)
    await input.setValue('1800')
    if (panel.status) panel.status.spindle.commanded_rpm = 1400
    await input.trigger(event === 'blur' ? 'blur' : 'keydown', { key: 'Escape' })
    expect(input.element.value).toBe('1400')
    expect(send).not.toHaveBeenCalled()
  })

  it('uses the current direction at confirmation, not the direction when editing began', async () => {
    const view = setup()
    await view.get(selector).setValue('1800')
    if (panel.status) panel.status.spindle.direction = 'reverse'
    await view.get(selector).trigger('keydown', { key: 'Enter' })
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'set_spindle', direction: 'reverse', rpm: 1800 })
  })

  it('keeps a stopped spindle stopped until an explicit direction button is pressed', async () => {
    const view = setup('stopped')
    await view.get(selector).setValue('1800')
    await view.get(selector).trigger('keydown', { key: 'Enter' })
    await view.get(selector).trigger('blur')
    expect(send).not.toHaveBeenCalled()
    const forward = view.findAll('button').find((button) => button.text() === 'Forward')
    await forward?.trigger('click')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'set_spindle', direction: 'forward', rpm: 1800 })
  })

  it.each(['', '0', '-1', 'no', 'Infinity'])('does not apply invalid RPM %j', async (value) => {
    const view = setup()
    await view.get(selector).setValue(value)
    await view.get(selector).trigger('keydown', { key: 'Enter' })
    expect(send).not.toHaveBeenCalled()
  })

  it.each(['stale', 'busy', 'estop', 'hello'] as const)('invalidates a draft after %s', async (change) => {
    const view = setup()
    const input = view.get<HTMLInputElement>(selector)
    await input.setValue('1800')
    if (change === 'stale') panel.link = 'stale'
    else if (change === 'hello') panel.hello = { ...hello }
    else if (panel.status) {
      if (change === 'busy') panel.status.interp = 'reading'
      else panel.status.machine = 'estop'
    }
    await nextTick()
    await input.trigger('keydown', { key: 'Enter' })
    expect(send).not.toHaveBeenCalled()
    expect(input.element.value).toBe('1200')
  })

  it('opens the keypad at commanded RPM and applies only its explicit Set RPM action', async () => {
    const view = setup('reverse')
    const input = view.get(selector)
    for (const type of ['pointerdown', 'pointerup']) {
      const event = new Event(type, { bubbles: true, cancelable: true })
      Object.defineProperties(event, { pointerType: { value: 'touch' }, pointerId: { value: 1 }, clientX: { value: 0 }, clientY: { value: 0 } })
      input.element.dispatchEvent(event)
    }
    await nextTick()
    expect(numericEntry.value?.initial).toBe('1200')
    expect(view.text()).toContain('Applies this RPM immediately')
    const dialog = view.get('[role="dialog"]')
    await dialog.get('input').setValue('1800')
    expect(send).not.toHaveBeenCalled()
    const confirm = dialog.findAll('button').find((button) => button.text() === 'Set RPM')
    expect(confirm).toBeDefined()
    await confirm?.trigger('pointerdown', { pointerType: 'touch' })
    await confirm?.trigger('pointerup', { pointerType: 'touch' })
    await confirm?.trigger('click')
    expect(send).toHaveBeenCalledExactlyOnceWith({ type: 'set_spindle', direction: 'reverse', rpm: 1800 })
    expect(numericEntry.value).toBeNull()
  })
})

