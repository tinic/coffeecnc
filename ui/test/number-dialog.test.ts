import { afterEach, describe, expect, it } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'

import NumberDialog from '../src/components/NumberDialog.vue'

let wrapper: VueWrapper | null = null
afterEach(() => { wrapper?.unmount(); wrapper = null })

function open(initial: string): VueWrapper {
  wrapper = mount(NumberDialog, { attachTo: document.body, global: { stubs: { teleport: true } },
    props: { title: 'Value', effect: 'Sets the value', label: 'Value', unit: 'mm', initial, action: 'Use value' } })
  return wrapper
}

describe('number keypad sign key', () => {
  it.each([['+5', -5], ['5', -5], ['-5', 5], ['2+3', -5]])('changes the sign of %s', async (initial, expected) => {
    const dialog = open(initial)
    const sign = dialog.findAll('.keypad button').find((button) => button.text() === '±')
    if (!sign) throw new Error('No sign key')
    await sign.trigger('click')
    const confirm = dialog.findAll('button').find((button) => button.text() === 'Use value')
    if (!confirm) throw new Error('No confirm button')
    await confirm.trigger('click')
    expect(dialog.emitted('confirm')?.[0]?.[0]).toBe(expected)
  })
})
