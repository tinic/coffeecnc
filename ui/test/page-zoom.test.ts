import { describe, expect, it } from 'vitest'
import { preventPageZoom } from '../src/page-zoom'

function touch(type: string, fingers: number): Event {
  const event = new Event(type, { cancelable: true })
  Object.defineProperty(event, 'touches', { value: { length: fingers } })
  return event
}

describe('page zoom', () => {
  // turo: a pinch on the touch screen zoomed the whole panel instead of the preview.
  it('handles two-finger touches so WebKit does not zoom the page; one finger is untouched', () => {
    const target = new EventTarget()
    preventPageZoom(target)
    for (const type of ['touchstart', 'touchmove']) {
      const pinch = touch(type, 2)
      target.dispatchEvent(pinch)
      expect(pinch.defaultPrevented).toBe(true)
      const drag = touch(type, 1)
      target.dispatchEvent(drag)
      expect(drag.defaultPrevented).toBe(false)
    }
    const gesture = new Event('gesturestart', { cancelable: true })
    target.dispatchEvent(gesture)
    expect(gesture.defaultPrevented).toBe(true)
  })
})
