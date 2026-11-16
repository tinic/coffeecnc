// Native GTK tooltips follow synthetic mouse positions after touch. Keep their text
// accessible, but expose the browser title only while mouse interaction is active.
import { shallowRef, watch, type ObjectDirective } from 'vue'
import { touchInput } from './numeric-input'

type Hint = string | null | undefined
const states = new WeakMap<HTMLElement, { value: ReturnType<typeof shallowRef<Hint>>; stop: () => void }>()
export const vTooltip: ObjectDirective<HTMLElement, Hint> = {
  mounted(element, binding) {
    const value = shallowRef(binding.value)
    const label = !element.hasAttribute('aria-label') && element.matches('button, [role="button"]') && element.textContent.trim() === ''
    const stop = watch([value, touchInput], ([hint, touch]) => {
      if (hint && !touch) element.setAttribute('title', hint)
      else element.removeAttribute('title')
      if (hint) element.setAttribute('aria-description', hint)
      else element.removeAttribute('aria-description')
      if (label) {
        if (hint) element.setAttribute('aria-label', hint)
        else element.removeAttribute('aria-label')
      }
    }, { immediate: true, flush: 'sync' })
    states.set(element, { value, stop })
  },
  updated(element, binding) { const state = states.get(element); if (state) state.value.value = binding.value },
  unmounted(element) { states.get(element)?.stop(); states.delete(element) },
}
