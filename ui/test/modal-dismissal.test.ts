import { afterEach, beforeEach, describe, expect, it } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { nextTick } from 'vue'
import ModalDialog from '../src/components/ModalDialog.vue'
import { touchInput, trackNumericInput } from '../src/numeric-input'

let wrapper: VueWrapper
let host: HTMLDivElement
let stopTracking: () => void
beforeEach(async () => {
  host = document.createElement('div')
  host.id = 'dialogs'
  document.body.append(host)
  touchInput.value = false
  stopTracking = trackNumericInput()
  wrapper = mount(ModalDialog, {
    props: { title: 'Confirm', size: 'small', freshGesture: true },
    slots: { default: '<button>Inside</button>' },
    attachTo: document.body,
  })
  await nextTick()
})
afterEach(() => { wrapper.unmount(); host.remove(); stopTracking(); touchInput.value = false })

describe('dialog backdrop dismissal', () => {
  it('closes on a pointer press on the backdrop', () => {
    host.querySelector('.scrim')?.dispatchEvent(new Event('pointerdown', { bubbles: true }))
    expect(wrapper.emitted('close')).toHaveLength(1)
  })

  it('closes on a touch-only backdrop gesture with fresh-gesture protection enabled', () => {
    const scrim = host.querySelector('.scrim')
    scrim?.dispatchEvent(new Event('touchstart', { bubbles: true }))
    expect(touchInput.value).toBe(true)
    expect(wrapper.emitted('close')).toBeUndefined()
    const end = new Event('touchend', { bubbles: true, cancelable: true })
    scrim?.dispatchEvent(end)
    expect(end.defaultPrevented).toBe(true)
    expect(wrapper.emitted('close')).toHaveLength(1)
  })

  it('does not dismiss when touching content inside the dialog', () => {
    const content = host.querySelector('.body button')
    for (const type of ['pointerdown', 'touchstart', 'touchend']) content?.dispatchEvent(new Event(type, { bubbles: true }))
    expect(wrapper.emitted('close')).toBeUndefined()
  })
})
