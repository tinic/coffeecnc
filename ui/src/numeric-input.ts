// Numeric touch entry never focuses an editable field. inputmode=none alone is insufficient
// on older WebKitGTK versions; readonly and prevented touch defaults also inhibit the OSK.
import { ref, shallowRef, watch, type ObjectDirective } from 'vue'

import { t } from './i18n'

export const touchInput = ref(false)
interface NumericEntry {
  key: number
  target: HTMLInputElement
  initial: string
  label: string
  integer: boolean
  unit?: string
  effect?: string
  action?: string
}
export const numericEntry = shallowRef<NumericEntry | null>(null)
let nextEntry = 0
let lastTouch = -Infinity
let gesture = 0

export function trackNumericInput(): () => void {
  const touch = (): void => { gesture += 1; lastTouch = Date.now(); touchInput.value = true }
  const pointer = (event: PointerEvent): void => {
    if (event.pointerType === 'touch' || event.pointerType === 'pen') touch()
    else if (Date.now() - lastTouch > 800) touchInput.value = false
  }
  const release = (event: Event): void => {
    if (event.type.startsWith('touch') || ['touch', 'pen'].includes((event as PointerEvent).pointerType)) {
      lastTouch = Date.now(); touchInput.value = true
    }
  }
  const endings = ['pointerup', 'pointercancel', 'touchend', 'touchcancel'] as const
  for (const event of endings) document.addEventListener(event, release, true)
  const keyboard = (): void => { touchInput.value = false }
  document.addEventListener('pointerdown', pointer, true)
  document.addEventListener('touchstart', touch, { capture: true, passive: true })
  document.addEventListener('keydown', keyboard, true)
  return (): void => {
    document.removeEventListener('pointerdown', pointer, true)
    document.removeEventListener('touchstart', touch, true)
    document.removeEventListener('keydown', keyboard, true)
    for (const event of endings) document.removeEventListener(event, release, true)
    numericEntry.value = null
  }
}

export function applyNumericEntry(value: number): boolean {
  const entry = numericEntry.value
  if (entry === null || !Number.isFinite(value) || entry.integer && !Number.isInteger(value)) return false
  const target = entry.target
  // Closing/replacing an editor or disabling/changing its field invalidates the old draft.
  if (!target.isConnected || target.disabled || target.value !== entry.initial) return false
  target.value = String(value)
  target.dispatchEvent(new Event('input', { bubbles: true }))
  target.dispatchEvent(new Event('change', { bubbles: true }))
  // Explicit keypad confirmation, distinct from a normal change/blur while typing.
  target.dispatchEvent(new Event('numeric-confirm', { bubbles: true }))
  numericEntry.value = null
  return true
}

const cleanup = new WeakMap<HTMLInputElement, () => void>()
/** Apply to every numeric text field; inline marks a field already inside NumberDialog. */
export const vNumeric: ObjectDirective<HTMLInputElement, 'inline' | 'integer' | undefined> = {
  mounted(input, binding) {
    const originallyReadonly = input.readOnly
    const inputMode = input.inputMode
    input.inputMode = 'none'
    const removeWatch = watch(touchInput, (touch) => { input.readOnly = originallyReadonly || touch }, { immediate: true, flush: 'sync' })
    let pointer: number | null = null
    let finger: number | null = null
    let finishedGesture = -1
    const open = (): void => {
      if (binding.value === 'inline' || numericEntry.value?.target === input) return
      numericEntry.value = {
        key: ++nextEntry, target: input, initial: input.value,
        label: input.getAttribute('aria-label') ?? input.labels?.[0]?.textContent.trim() ?? t('status.number.number'),
        integer: binding.value === 'integer' || inputMode === 'numeric',
        ...(input.dataset['numericUnit'] ? { unit: input.dataset['numericUnit'] } : {}),
        ...(input.dataset['numericEffect'] ? { effect: input.dataset['numericEffect'] } : {}),
        ...(input.dataset['numericAction'] ? { action: input.dataset['numericAction'] } : {}),
      }
    }
    const inside = (point: { clientX: number; clientY: number }): boolean => {
      const box = input.getBoundingClientRect()
      return point.clientX >= box.left && point.clientX <= box.right && point.clientY >= box.top && point.clientY <= box.bottom
    }
    const touch = (event: Event): void => {
      if (event.type.endsWith('cancel')) {
        pointer = null; finger = null; finishedGesture = gesture
        return
      }
      if (input.disabled || originallyReadonly) return
      if (event.type === 'touchstart' || event.type === 'pointerdown' && ['touch', 'pen'].includes((event as PointerEvent).pointerType)) {
        lastTouch = Date.now()
        touchInput.value = true
      }
      if (!touchInput.value) return
      event.preventDefault()
      input.readOnly = true
      if (document.activeElement instanceof HTMLElement) document.activeElement.blur()
      if (event.type === 'pointerdown') {
        if ((event as PointerEvent).pointerType !== 'mouse') pointer = (event as PointerEvent).pointerId
        finishedGesture = -1
      } else if (event.type === 'touchstart') {
        finger = (event as TouchEvent).changedTouches[0]?.identifier ?? null
        finishedGesture = -1
      } else if (event.type === 'pointerup') {
        const released = event as PointerEvent
        const complete = pointer === released.pointerId && finger === null
        pointer = null
        if (complete) { finishedGesture = gesture; if (inside(released)) open() }
      } else if (event.type === 'touchend') {
        const released = Array.from((event as TouchEvent).changedTouches).find((point) => point.identifier === finger)
        if (released !== undefined) {
          finger = null; pointer = null; finishedGesture = gesture
          if (inside(released)) open()
        }
      } else if (event.type === 'click' && finishedGesture !== gesture) {
        // A label may activate the readonly field without a press on the input itself.
        // Ignore the compatibility click belonging to an already-ended field gesture.
        finishedGesture = gesture
        open()
      }
    }
    const events = ['pointerdown', 'pointerup', 'pointercancel', 'touchstart', 'touchend', 'touchcancel', 'click'] as const
    for (const event of events) input.addEventListener(event, touch, { passive: false })
    cleanup.set(input, () => {
      removeWatch()
      for (const event of events) input.removeEventListener(event, touch)
      if (numericEntry.value?.target === input) numericEntry.value = null
    })
  },
  unmounted(input) { cleanup.get(input)?.(); cleanup.delete(input) },
}

// A release/click from the gesture that opened a dialog must never activate its controls.
// Capture runs before Vue click handlers, including the dialog's Close and confirm buttons.
const activationCleanup = new WeakMap<HTMLElement, () => void>()
export const vFreshGesture: ObjectDirective<HTMLElement, boolean | undefined> = {
  mounted(root, binding) {
    if (!binding.value) return
    let pressed: Element | null = null
    const control = (event: Event): Element | null => event.target instanceof Element
      ? event.target.closest('button, input[data-native-text-input]') : null // i18n-ignore: selector
    const start = (event: Event): void => {
      const target = control(event)
      if (event.type === 'pointerdown' && touchInput.value && (event as PointerEvent).pointerType === 'mouse') {
        // WebKit can synthesize mouse events after touch release over newly inserted content.
        // A text field pressed by this touch needs the mouse default to receive native focus.
        if (!(target instanceof HTMLInputElement) || target !== pressed) {
          event.preventDefault(); event.stopImmediatePropagation()
        }
        return
      }
      pressed = target
    }
    const cancel = (): void => { pressed = null }
    const click = (event: Event): void => {
      const allowed = !touchInput.value || pressed !== null && pressed === control(event)
      pressed = null
      if (!allowed) { event.preventDefault(); event.stopImmediatePropagation() }
    }
    root.addEventListener('pointerdown', start, true)
    root.addEventListener('touchstart', start, { capture: true, passive: true })
    root.addEventListener('pointercancel', cancel, true)
    root.addEventListener('touchcancel', cancel, true)
    root.addEventListener('click', click, true)
    activationCleanup.set(root, () => {
      root.removeEventListener('pointerdown', start, true)
      root.removeEventListener('touchstart', start, true)
      root.removeEventListener('pointercancel', cancel, true)
      root.removeEventListener('touchcancel', cancel, true)
      root.removeEventListener('click', click, true)
    })
  },
  unmounted(root) { activationCleanup.get(root)?.(); activationCleanup.delete(root) },
}
