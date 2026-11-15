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
const fixture = defineComponent({
  components: { NumericEntryDialog }, directives: { numeric: vNumeric },
  setup: () => ({ value, disabled, visible, integer }),
  template: `<div><label v-if="visible">Spindle speed, rpm
    <input data-source v-model="value" v-numeric="integer ? 'integer' : undefined" :disabled="disabled" type="text" inputmode="decimal" />
    </label><div id="dialogs"></div><div id="workspace-dialogs"></div><NumericEntryDialog /></div>`,
})
let wrapper: VueWrapper
function press(label: string): Promise<void> {
  const button = wrapper.findAll('button').find((b) => b.text() === label)
  if (button === undefined) throw new Error(`No button ${label}`)
  return button.trigger('click')
}
async function touch(type = 'pointerdown'): Promise<Event> {
  const event = new Event(type, { bubbles: true, cancelable: true })
  if (type === 'pointerdown') Object.defineProperty(event, 'pointerType', { value: 'touch' })
  wrapper.get('[data-source]').element.dispatchEvent(event)
  await nextTick()
  return event
}
beforeEach(() => {
  value.value = '12.5'; disabled.value = false; visible.value = true; integer.value = false
  touchInput.value = false; numericEntry.value = null
  panel.unit = 'mm'; panel.tab = 'manual'; panel.hello = null
  wrapper = mount(fixture, { attachTo: document.body })
})
afterEach(() => { const element = wrapper.element as HTMLElement; wrapper.unmount(); element.remove(); vi.restoreAllMocks() })

describe('numeric touch entry', () => {
  it.each(['pointerdown', 'touchstart'])('prevents %s focus and edits only the draft until Use value', async (type) => {
    const source = wrapper.get('[data-source]').element as HTMLInputElement
    const event = await touch(type)
    expect(event.defaultPrevented).toBe(true)
    expect(source.readOnly).toBe(true)
    expect(source.inputMode).toBe('none')
    expect(document.activeElement?.tagName).not.toBe('INPUT')
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
