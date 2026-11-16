import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { defineComponent, nextTick, ref } from 'vue'
import NumericEntryDialog from '../src/components/NumericEntryDialog.vue'
import { applyNumericEntry, numericEntry, touchInput, vNumeric } from '../src/numeric-input'
import { openDialogs } from '../src/dialogs'
import { panel } from '../src/machine'

vi.mock('../src/machine', async () => {
  const { reactive } = await import('vue')
  return { panel: reactive({ hello: null, unit: 'mm', tab: 'manual' }) }
})
const value = ref('12.5')
const disabled = ref(false)
const visible = ref(true)
const integer = ref(false)
const fieldUnit = ref('')
const fixture = defineComponent({
  components: { NumericEntryDialog }, directives: { numeric: vNumeric },
  setup: () => ({ value, disabled, visible, integer, fieldUnit }),
  template: `<div><label v-if="visible">Spindle speed, rpm
    <input data-source v-model="value" v-numeric="integer ? 'integer' : undefined" :data-numeric-unit="fieldUnit" :disabled="disabled" type="text" inputmode="decimal" />
    </label><div id="dialogs"></div><div id="workspace-dialogs"></div><NumericEntryDialog /></div>`,
})
let wrapper: VueWrapper
function fieldEvent(type: string, point = 0): Event {
  const event = new Event(type, { bubbles: true, cancelable: true })
  if (type.startsWith('pointer')) {
    Object.defineProperties(event, { pointerType: { value: 'touch' }, pointerId: { value: 7 }, clientX: { value: point }, clientY: { value: point } })
  } else if (type.startsWith('touch')) {
    Object.defineProperty(event, 'changedTouches', { value: [{ identifier: 7, clientX: point, clientY: point }] })
  }
  return event
}
async function activate(button: { element: Element }): Promise<void> {
  button.element.dispatchEvent(fieldEvent('pointerdown'))
  button.element.dispatchEvent(fieldEvent('pointerup'))
  button.element.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true, detail: 1 }))
  await nextTick()
}
function press(label: string): Promise<void> {
  const button = wrapper.findAll('button').find((b) => b.text() === label)
  if (button === undefined) throw new Error(`No button ${label}`)
  return activate(button)
}
async function touch(type = 'pointerdown', release = true): Promise<Event> {
  const event = fieldEvent(type)
  wrapper.get('[data-source]').element.dispatchEvent(event)
  await nextTick()
  if (release) {
    wrapper.get('[data-source]').element.dispatchEvent(fieldEvent(type === 'touchstart' ? 'touchend' : 'pointerup'))
    await nextTick()
  }
  return event
}
beforeEach(() => {
  value.value = '12.5'; disabled.value = false; visible.value = true; integer.value = false; fieldUnit.value = ''
  touchInput.value = false; numericEntry.value = null
  panel.unit = 'mm'; panel.tab = 'manual'; panel.hello = null
  wrapper = mount(fixture, { attachTo: document.body })
})
afterEach(() => { const element = wrapper.element as HTMLElement; wrapper.unmount(); element.remove(); vi.restoreAllMocks() })

describe('numeric touch entry', () => {
  it('enters a fraction and inch suffix by touch, previews it and confirms in displayed mm', async () => {
    fieldUnit.value = 'mm'; await nextTick()
    await touch()
    for (const key of ['1', '÷', '4', 'in']) await press(key)
    expect(wrapper.get('output').text()).toBe('= 6.35 mm')
    expect(value.value).toBe('12.5')
    await press('Use value')
    expect(value.value).toBe('6.35')
  })
  it.each(['pointerdown', 'touchstart'])('prevents %s focus and edits only the draft until Use value', async (type) => {
    const source = wrapper.get('[data-source]').element as HTMLInputElement
    const event = await touch(type, false)
    expect(event.defaultPrevented).toBe(true)
    expect(source.readOnly).toBe(true)
    expect(source.inputMode).toBe('none')
    expect(document.activeElement?.tagName).not.toBe('INPUT')
    expect(openDialogs.value).toBe(0)
    source.dispatchEvent(fieldEvent(type === 'touchstart' ? 'touchend' : 'pointerup'))
    await nextTick()
    expect(openDialogs.value).toBe(1)
    expect(wrapper.get('[role="dialog"]').attributes('aria-label')).toBe('Spindle speed, rpm')
    await press('4'); await press('.'); await press('2'); await press('±')
    expect(value.value).toBe('12.5')
    await press('Use value')
    expect(value.value).toBe('-4.2')
    expect(wrapper.find('[role="dialog"]').exists()).toBe(false)
    expect(openDialogs.value).toBe(0)
  })
  it('cancels without changing the source field, including compatibility clicks', async () => {
    await touch(); await wrapper.get('[data-source]').trigger('click')
    expect(wrapper.findAll('[role="dialog"]')).toHaveLength(1)
    await press('9'); await press('Cancel')
    expect(value.value).toBe('12.5')
  })
  it('does not turn the opening release or compatibility clicks into keypad actions', async () => {
    value.value = '600'; await nextTick()
    await touch('touchstart')
    const backspace = wrapper.get('button[aria-label="Delete the last digit"]')
    // Real-screen regression: the release is retargeted to the newly inserted keypad.
    backspace.element.dispatchEvent(fieldEvent('touchend'))
    backspace.element.dispatchEvent(fieldEvent('pointerup'))
    await backspace.trigger('mousedown'); await backspace.trigger('mouseup'); await backspace.trigger('click')
    expect((wrapper.get('[role="dialog"] input').element as HTMLInputElement).value).toBe('600')
    // No new press: digits, corrections, confirmation, cancellation and Close all ignore it.
    for (const button of wrapper.findAll('[role="dialog"] button')) await button.trigger('click')
    expect(wrapper.find('[role="dialog"]').exists()).toBe(true)
    expect(value.value).toBe('600')
    expect((wrapper.get('[role="dialog"] input').element as HTMLInputElement).value).toBe('600')
    await activate(backspace)
    expect((wrapper.get('[role="dialog"] input').element as HTMLInputElement).value).toBe('60')
    await backspace.trigger('click')
    expect((wrapper.get('[role="dialog"] input').element as HTMLInputElement).value).toBe('60')
    await press('Use value'); expect(value.value).toBe('60')
  })
  it.each(['pointerdown', 'touchstart'])('cancels %s without opening on a later compatibility click', async (start) => {
    await touch(start, false)
    const source = wrapper.get('[data-source]')
    source.element.dispatchEvent(fieldEvent(start === 'touchstart' ? 'touchcancel' : 'pointercancel'))
    source.element.dispatchEvent(fieldEvent(start === 'touchstart' ? 'touchend' : 'pointerup'))
    await source.trigger('click')
    expect(numericEntry.value).toBeNull()
  })
  it('rejects compatibility mouse events after a long opening touch', async () => {
    let now = 1000
    vi.spyOn(Date, 'now').mockImplementation(() => now)
    value.value = '600'; await nextTick()
    await touch('touchstart', false)
    now += 2000
    wrapper.get('[data-source]').element.dispatchEvent(fieldEvent('touchend')); await nextTick()
    const backspace = wrapper.get('button[aria-label="Delete the last digit"]')
    await backspace.trigger('pointerdown', { pointerType: 'mouse' })
    await backspace.trigger('click')
    expect((wrapper.get('[role="dialog"] input').element as HTMLInputElement).value).toBe('600')
    expect(touchInput.value).toBe(true)
  })
  it('waits for touchend when WebKit supplies both pointer and touch events', async () => {
    await touch('pointerdown', false); await touch('touchstart', false)
    const source = wrapper.get('[data-source]')
    source.element.dispatchEvent(fieldEvent('pointerup')); await nextTick()
    expect(numericEntry.value).toBeNull()
    source.element.dispatchEvent(fieldEvent('touchend')); await nextTick()
    expect(wrapper.findAll('[role="dialog"]')).toHaveLength(1)
    await source.trigger('click')
    expect(wrapper.findAll('[role="dialog"]')).toHaveLength(1)
  })
  it.each(['pointerdown', 'touchstart'])('does not open when %s is released outside the field', async (start) => {
    await touch(start, false)
    const source = wrapper.get('[data-source]')
    source.element.dispatchEvent(fieldEvent(start === 'touchstart' ? 'touchend' : 'pointerup', 100))
    await source.trigger('click')
    expect(numericEntry.value).toBeNull()
  })
  it('allows keyboard activation after using touch', async () => {
    await touch()
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Tab', bubbles: true }))
    const digit = wrapper.findAll('button').find((b) => b.text() === '9')
    await digit?.trigger('click')
    expect((wrapper.get('[role="dialog"] input').element as HTMLInputElement).value).toBe('9')
  })
  it('leaves physical-keyboard entry available', async () => {
    document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Tab', bubbles: true }))
    await wrapper.get('[data-source]').setValue('123.45')
    expect(value.value).toBe('123.45')
    expect(numericEntry.value).toBeNull()
    expect((wrapper.get('[data-source]').element as HTMLInputElement).readOnly).toBe(false)
  })
  it('supports integer fields without accepting fractions', async () => {
    // Binding mode is read when mounted, matching each production field's stable type.
    const element = wrapper.element as HTMLElement
    wrapper.unmount(); element.remove(); integer.value = true
    wrapper = mount(fixture, { attachTo: document.body })
    await touch()
    expect(wrapper.findAll('button').find((b) => b.text() === '.')?.attributes('disabled')).toBeDefined()
    expect(wrapper.findAll('button').find((b) => b.text() === 'Use value')?.attributes('disabled')).toBeDefined()
    await press('2'); await press('Use value'); expect(value.value).toBe('2')
  })
  it('does not open disabled fields', async () => {
    disabled.value = true; await nextTick(); await touch()
    expect(numericEntry.value).toBeNull()
  })
  it.each([NaN, Infinity, -Infinity])('does not apply non-finite value %s', async (invalid) => {
    await touch()
    expect(applyNumericEntry(invalid)).toBe(false)
    expect(value.value).toBe('12.5')
  })
  it.each(['changed', 'disabled', 'removed', 'units', 'workspace'])('does not apply a draft after its context is %s', async (change) => {
    await touch(); await press('8')
    if (change === 'changed') value.value = '27'
    else if (change === 'disabled') disabled.value = true
    else if (change === 'removed') visible.value = false
    else if (change === 'units') panel.unit = 'inch'
    else panel.tab = 'tools'
    await nextTick()
    if (wrapper.find('[role="dialog"]').exists()) {
      await press('Use value')
      expect(wrapper.get('[role="alert"]').text()).toContain('field changed')
    }
    expect(value.value).toBe(change === 'changed' ? '27' : '12.5')
  })
})
