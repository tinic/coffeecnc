import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { defineComponent } from 'vue'
import { vPopover } from '../src/popover'

let wrapper: VueWrapper
let menu: HTMLDetailsElement
let outside: HTMLButtonElement
const action = vi.fn()
const Fixture = defineComponent({
  directives: { popover: vPopover },
  setup: () => ({ action }),
  template: `<div>
    <details v-popover><summary>Menu</summary><div><button @click="action">Inside action</button><input /></div></details>
    <details v-popover><summary>Other menu</summary><div>Other content</div></details>
    <button class="outside" @pointerdown.stop="action" @click="action">Stop</button>
  </div>`,
})

beforeEach(() => {
  action.mockReset()
  wrapper = mount(Fixture, { attachTo: document.body })
  menu = wrapper.get('details').element
  outside = wrapper.get('.outside').element as HTMLButtonElement
  menu.open = true
})
afterEach(() => { wrapper.unmount() })

describe('popover dismissal', () => {
  it.each(['pointerdown', 'touchstart', 'click'])('closes on outside %s without blocking its target', (type) => {
    const event = new Event(type, { bubbles: true, cancelable: true })
    outside.dispatchEvent(event)
    expect(menu.open).toBe(false)
    expect(event.defaultPrevented).toBe(false)
    if (type !== 'touchstart') expect(action).toHaveBeenCalledOnce()
  })

  it('stays closed through WebKit touch and compatibility events', () => {
    for (const type of ['touchstart', 'touchend', 'pointerdown', 'pointerup', 'click']) {
      outside.dispatchEvent(new Event(type, { bubbles: true }))
      expect(menu.open).toBe(false)
    }
  })

  it('keeps inside controls and their actions usable', async () => {
    const inside = wrapper.get('details button')
    for (const type of ['pointerdown', 'touchstart', 'click']) {
      await inside.trigger(type)
      expect(menu.open).toBe(true)
    }
    await wrapper.get('input').trigger('pointerdown')
    expect(menu.open).toBe(true)
    expect(action).toHaveBeenCalledOnce()
  })

  it('closes the previous menu when another summary is activated', () => {
    const second = wrapper.findAll('details')[1]?.element as HTMLDetailsElement
    second.querySelector('summary')?.click()
    expect(menu.open).toBe(false)
    expect(second.open).toBe(true)
  })

  it('closes on Escape and returns focus to its summary', async () => {
    const input = wrapper.get('input')
    ;(input.element as HTMLInputElement).focus()
    await input.trigger('keydown', { key: 'Escape' })
    expect(menu.open).toBe(false)
    expect(document.activeElement).toBe(menu.querySelector('summary'))
  })

  it('removes document listeners when unmounted', () => {
    wrapper.unmount()
    menu.open = true
    document.body.dispatchEvent(new Event('pointerdown', { bubbles: true }))
    expect(menu.open).toBe(true)
  })
})
