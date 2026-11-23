import type { ObjectDirective } from 'vue'

const cleanup = new WeakMap<HTMLDetailsElement, () => void>()

/** Light dismissal for the app, coordinate and execution menus. */
export const vPopover: ObjectDirective<HTMLDetailsElement> = {
  mounted(menu) {
    const owner = menu.ownerDocument
    const closeOutside = (event: Event): void => {
      if (menu.open && !event.composedPath().includes(menu)) menu.open = false
    }
    const escape = (event: KeyboardEvent): void => {
      if (event.key !== 'Escape' || !menu.open) return // i18n-ignore: key name
      menu.open = false
      event.preventDefault()
      event.stopPropagation()
      menu.querySelector('summary')?.focus({ preventScroll: true })
    }
    // WebKitGTK touch screens can report touch events before synthetic pointer events.
    // Capture also sees taps whose target stops propagation; never swallow Stop/E-stop.
    const events = ['pointerdown', 'touchstart', 'click'] as const
    for (const event of events) owner.addEventListener(event, closeOutside, { capture: true, passive: true })
    menu.addEventListener('keydown', escape)
    cleanup.set(menu, () => {
      for (const event of events) owner.removeEventListener(event, closeOutside, true)
      menu.removeEventListener('keydown', escape)
    })
  },
  unmounted(menu) { cleanup.get(menu)?.(); cleanup.delete(menu) },
}
