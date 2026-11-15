import { afterEach, describe, expect, it } from 'vitest'
import { mount } from '@vue/test-utils'
import { defineComponent, nextTick, ref } from 'vue'
import { vTooltip } from '../src/tooltip'
import { touchInput } from '../src/numeric-input'

afterEach(() => { touchInput.value = false })
describe('touch tooltip suppression', () => {
  it('removes native titles, preserves accessible labels and updates changing hints', async () => {
    const hint = ref<string | undefined>('Close')
    const wrapper = mount(defineComponent({ directives: { tooltip: vTooltip }, setup: () => ({ hint }),
      template: '<div><button v-tooltip="hint"></button><button v-tooltip="hint" aria-label="Explicit label">Text</button></div>' }))
    expect(wrapper.get('button').attributes('title')).toBe('Close')
    touchInput.value = true
    expect(wrapper.find('[title]').exists()).toBe(false)
    expect(wrapper.get('button').attributes('aria-label')).toBe('Close')
    hint.value = 'Updated hint'; await nextTick()
    expect(wrapper.find('[title]').exists()).toBe(false)
    expect(wrapper.get('button').attributes('aria-label')).toBe('Updated hint')
    expect(wrapper.findAll('button')[1]?.attributes('aria-label')).toBe('Explicit label')
    touchInput.value = false
    expect(wrapper.get('button').attributes('title')).toBe('Updated hint')
    hint.value = undefined; await nextTick()
    expect(wrapper.find('[title]').exists()).toBe(false)
    expect(wrapper.find('[aria-description]').exists()).toBe(false)
    wrapper.unmount()
  })
})
