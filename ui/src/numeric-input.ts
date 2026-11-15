// Numeric touch entry never focuses an editable field. inputmode=none alone is insufficient
// on older WebKitGTK versions; readonly and prevented touch defaults also inhibit the OSK.
import { ref, shallowRef, watch, type ObjectDirective } from 'vue'

export const touchInput = ref(false)
interface NumericEntry {
  key: number
  target: HTMLInputElement
  initial: string
  label: string
  integer: boolean
}
export const numericEntry = shallowRef<NumericEntry | null>(null)
let nextEntry = 0
let lastTouch = -Infinity

export function trackNumericInput(): () => void {
  const touch = (): void => { lastTouch = Date.now(); touchInput.value = true }
  const pointer = (event: PointerEvent): void => {
    if (event.pointerType === 'touch' || event.pointerType === 'pen') touch()
    else if (Date.now() - lastTouch > 800) touchInput.value = false
  }
  const keyboard = (): void => { touchInput.value = false }
  document.addEventListener('pointerdown', pointer, true)
  document.addEventListener('touchstart', touch, { capture: true, passive: true })
  document.addEventListener('keydown', keyboard, true)
  return (): void => {
    document.removeEventListener('pointerdown', pointer, true)
    document.removeEventListener('touchstart', touch, true)
    document.removeEventListener('keydown', keyboard, true)
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
    const touch = (event: Event): void => {
      if (input.disabled || originallyReadonly) return
      if (event.type === 'touchstart' || event.type === 'pointerdown' && ['touch', 'pen'].includes((event as PointerEvent).pointerType)) {
        lastTouch = Date.now()
        touchInput.value = true
      }
      if (!touchInput.value) return
      event.preventDefault()
      input.readOnly = true
      if (document.activeElement instanceof HTMLElement) document.activeElement.blur()
      if (binding.value === 'inline' || numericEntry.value?.target === input) return
      numericEntry.value = {
        key: ++nextEntry, target: input, initial: input.value,
        label: input.getAttribute('aria-label') ?? input.labels?.[0]?.textContent.trim() ?? 'Number',
        integer: binding.value === 'integer' || inputMode === 'numeric',
      }
    }
    input.addEventListener('pointerdown', touch)
    input.addEventListener('touchstart', touch, { passive: false })
    // Also handle a label activating its input and touch-generated compatibility clicks.
    input.addEventListener('click', touch)
    cleanup.set(input, () => {
      removeWatch()
      input.removeEventListener('pointerdown', touch)
      input.removeEventListener('touchstart', touch)
      input.removeEventListener('click', touch)
      if (numericEntry.value?.target === input) numericEntry.value = null
    })
  },
  unmounted(input) { cleanup.get(input)?.(); cleanup.delete(input) },
}
