import { describe, expect, it } from 'vitest'

import {
  JogHold,
  Keys,
  Touches,
  capturedPress,
  emulatedMouseWindowMs,
  refreshMs,
  type CaptureTarget,
  type Timers,
} from '../src/jogHold'
import { deadmanMs } from '../src/sim/machine'

type Key = 'x+' | 'x-' | 'z+' | 'z-'

class FakeTimers implements Timers {
  private next = 1
  readonly active = new Map<number, () => void>()

  setInterval(callback: () => void): number {
    const handle = this.next
    this.next += 1
    this.active.set(handle, callback)
    return handle
  }

  clearInterval(handle: number): void {
    this.active.delete(handle)
  }

  tick(): void {
    for (const callback of [...this.active.values()]) {
      callback()
    }
  }
}

/** A button the mouse pointer is on (down) or not; capture works only while the pointer is down. */
class FakeButton implements CaptureTarget {
  down = true
  captured = false

  setPointerCapture(): void {
    if (!this.down) {
      throw new DOMException('pointer is not active', 'NotFoundError')
    }
    this.captured = true
  }

  hasPointerCapture(): boolean {
    return this.captured && this.down
  }

  lift(): void {
    this.down = false
    this.captured = false
  }
}

function setup(): { hold: JogHold<Key>; timers: FakeTimers; sent: [Key, boolean][] } {
  const sent: [Key, boolean][] = []
  const timers = new FakeTimers()
  const hold = new JogHold<Key>(
    {
      send: (key: Key, held: boolean): void => {
        sent.push([key, held])
      },
    },
    timers,
  )
  return { hold, timers, sent }
}

const never = (): boolean => false

describe('jog hold with a mouse', () => {
  it('jogs while the button is held and stops on release', () => {
    const { hold, timers, sent } = setup()
    const stillHeld = capturedPress(new FakeButton(), 1)
    expect(stillHeld).not.toBeNull()
    expect(hold.press('z-', stillHeld ?? never)).toBe(true)
    expect(hold.any).toBe(true)
    timers.tick()
    timers.tick()
    hold.release('z-')
    expect(sent).toEqual([
      ['z-', true],
      ['z-', true],
      ['z-', true],
      ['z-', false],
    ])
    expect(timers.active.size).toBe(0)
    expect(hold.any).toBe(false)
  })

  it('does not jog when the press arrives after the pointer was already released', () => {
    const { hold, sent } = setup()
    const button = new FakeButton()
    button.lift()
    expect(capturedPress(button, 1)).toBeNull()
    expect(hold.press('x+', never)).toBe(false)
    expect(sent).toEqual([])
  })

  it('stops by itself when the release event is lost', () => {
    const { hold, timers, sent } = setup()
    const button = new FakeButton()
    hold.press('x+', capturedPress(button, 7) ?? never)
    button.lift() // no pointerup, no pointercancel
    timers.tick()
    expect(sent).toEqual([
      ['x+', true],
      ['x+', false],
    ])
    expect(hold.held('x+')).toBe(false)
    timers.tick()
    expect(sent).toHaveLength(2)
  })

  it('a second press of the same control restarts it with one release in between', () => {
    const { hold, timers, sent } = setup()
    hold.press('x+', () => true)
    hold.press('x+', () => true)
    expect(sent).toEqual([
      ['x+', true],
      ['x+', false],
      ['x+', true],
    ])
    expect(timers.active.size).toBe(1)
  })
})

describe('jog hold with touch', () => {
  it('jogs from touchstart to touchend', () => {
    const { hold, timers, sent } = setup()
    const touches = new Touches()
    hold.press('z-', touches.start(3, 1000))
    timers.tick()
    touches.end(3, 1500)
    hold.release('z-')
    expect(sent).toEqual([
      ['z-', true],
      ['z-', true],
      ['z-', false],
    ])
  })

  it('stops by itself when the touch ended elsewhere', () => {
    const { hold, timers, sent } = setup()
    const touches = new Touches()
    hold.press('z+', touches.start(5, 0))
    touches.end(5, 100) // seen by the window-level listener, not by the button
    timers.tick()
    expect(sent).toEqual([
      ['z+', true],
      ['z+', false],
    ])
  })

  it('treats the mouse events WebKit emulates after a touch as part of that touch', () => {
    // Recorded on a machine: touchstart 52685, touchend 53156, then pointerdown/up at 53157-53160.
    const touches = new Touches()
    touches.start(1, 52685)
    touches.end(1, 53156)
    expect(touches.emulatingMouse(53157)).toBe(true)
    expect(touches.emulatingMouse(53160)).toBe(true)
    expect(touches.emulatingMouse(53156 + emulatedMouseWindowMs)).toBe(false)
  })

  it('takes mouse events as a real mouse when no touch happened', () => {
    expect(new Touches().emulatingMouse(0)).toBe(false)
  })

  it('a touch on stop also marks its emulated mouse events', () => {
    const touches = new Touches()
    touches.touched(2000)
    expect(touches.emulatingMouse(2005)).toBe(true)
  })
})

describe('jog hold with the keyboard', () => {
  it('jogs from key down to key up', () => {
    const { hold, timers, sent } = setup()
    const keys = new Keys()
    hold.press('x+', keys.press('ArrowRight'))
    timers.tick()
    keys.release('ArrowRight')
    hold.release('x+')
    expect(sent).toEqual([
      ['x+', true],
      ['x+', true],
      ['x+', false],
    ])
  })

  it('stops by itself when the key-up never arrives because the window lost the focus', () => {
    const { hold, timers, sent } = setup()
    const keys = new Keys()
    hold.press('x+', keys.press('ArrowRight'))
    hold.press('z-', keys.press('PageDown'))
    keys.clear()
    timers.tick()
    expect(sent.filter(([, held]) => !held)).toEqual([
      ['x+', false],
      ['z-', false],
    ])
    expect(hold.any).toBe(false)
  })
})

describe('jog hold', () => {
  it('stop releases every held control', () => {
    const { hold, sent } = setup()
    const touches = new Touches()
    hold.press('z+', touches.start(1, 0))
    hold.press('x-', touches.start(2, 0))
    hold.releaseAll()
    expect(sent.filter(([, held]) => !held)).toEqual([
      ['z+', false],
      ['x-', false],
    ])
  })

  it('refreshes faster than the machine-side dead-man time', () => {
    expect(refreshMs).toBeLessThan(deadmanMs / 2)
  })
})
