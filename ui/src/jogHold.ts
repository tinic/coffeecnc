// Press-and-hold jogging. While a jog control is held, the panel re-sends "held" every refreshMs;
// the machine stops the axis by itself when those refreshes stop for 150 ms. The panel side
// therefore must never keep refreshing without a finger, mouse button or key actually down, so
// every press comes with a check that it is still held, and every refresh asks it first:
//
// - Mouse: the press captures the pointer; it is held while the button keeps the capture. If the
//   pointer is no longer down when the press arrives, capture fails and no jog starts.
// - Touch: WebKitGTK reports a touch at once only as touchstart; pointer and mouse events follow
//   after the finger lifts, as an emulated tap (seen on a machine over remote desktop:
//   touchstart, touchend, then pointerdown/pointerup/click within 3 ms). Touches are therefore
//   tracked by identifier from touchstart to touchend/touchcancel, and the emulated mouse events
//   that may still follow a touch are ignored.
// - Keyboard: held while the key is in the set of keys that are down.

export const refreshMs = 50

/** How long after a touch mouse events are taken to be WebKit's emulation of that touch. */
export const emulatedMouseWindowMs = 800

export interface JogSink<K> {
  send(key: K, held: boolean): void
}

export interface Timers {
  setInterval(callback: () => void, ms: number): number
  clearInterval(handle: number): void
}

/** The parts of a pressed element a mouse press needs: pointer capture. */
export interface CaptureTarget {
  setPointerCapture(pointerId: number): void
  hasPointerCapture(pointerId: number): boolean
}

export class JogHold<K> {
  private readonly holds = new Map<K, number>()

  constructor(
    private readonly sink: JogSink<K>,
    private readonly timers: Timers,
  ) {}

  /** Starts jogging while stillHeld() is true. Returns whether a jog started. */
  press(key: K, stillHeld: () => boolean): boolean {
    this.release(key)
    if (!stillHeld()) {
      return false
    }
    const timer = this.timers.setInterval(() => {
      if (stillHeld()) {
        this.sink.send(key, true)
      } else {
        this.release(key)
      }
    }, refreshMs)
    this.holds.set(key, timer)
    // A transport can reject synchronously. Register the hold before sending so its result
    // listener can release it without leaving a refresh timer behind.
    this.sink.send(key, true)
    return this.holds.has(key)
  }

  release(key: K): void {
    const timer = this.holds.get(key)
    if (timer === undefined) {
      return
    }
    this.timers.clearInterval(timer)
    this.holds.delete(key)
    this.sink.send(key, false)
  }

  releaseAll(): void {
    for (const key of [...this.holds.keys()]) {
      this.release(key)
    }
  }

  held(key: K): boolean {
    return this.holds.has(key)
  }

  get any(): boolean {
    return this.holds.size > 0
  }
}

/**
 * A mouse press: held while the element keeps the pointer capture. Returns null when the pointer
 * cannot be captured because it is not down (any more).
 */
export function capturedPress(target: CaptureTarget, pointerId: number): (() => boolean) | null {
  try {
    target.setPointerCapture(pointerId)
  } catch {
    return null
  }
  return () => target.hasPointerCapture(pointerId)
}

/** The touches currently down, by identifier, and when the last touch began or ended. */
export class Touches {
  private readonly down = new Set<number>()
  private lastActivity = Number.NEGATIVE_INFINITY

  start(identifier: number, now: number): () => boolean {
    this.down.add(identifier)
    this.lastActivity = now
    return () => this.down.has(identifier)
  }

  /** A touch that is not a jog press (stop, say): only its emulated mouse events must be ignored. */
  touched(now: number): void {
    this.lastActivity = now
  }

  end(identifier: number, now: number): void {
    this.down.delete(identifier)
    this.lastActivity = now
  }

  /** Mouse events this soon after a touch are WebKit's emulation of it, not a mouse. */
  emulatingMouse(now: number): boolean {
    return now - this.lastActivity < emulatedMouseWindowMs
  }
}

/** The keys currently down, for keyboard jogging. */
export class Keys {
  private readonly down = new Set<string>()

  press(code: string): () => boolean {
    this.down.add(code)
    return () => this.down.has(code)
  }

  release(code: string): void {
    this.down.delete(code)
  }

  /** Focus left the panel: no key-up will arrive, so no key counts as down. */
  clear(): void {
    this.down.clear()
  }
}
