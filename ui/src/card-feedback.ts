import { computed, shallowRef, watch, type ComputedRef, type WatchSource } from 'vue'
import type { SetterStatusLine } from './setter-state'

/** Keep feedback local to the pressed control's card and clear it when eligibility changes. */
export function useCardFeedback(
  hint: () => string,
  localBlock: () => string | null,
  sources: WatchSource<unknown>[],
): { line: ComputedRef<SetterStatusLine>; allow: (reason: string | null) => boolean } {
  const attempted = shallowRef<string | null>(null)
  watch(sources, () => { attempted.value = null })
  const line = computed((): SetterStatusLine => {
    const reason = attempted.value ?? localBlock()
    return reason === null ? { text: hint(), level: 'info' } : { text: reason, level: 'blocked' }
  })
  function allow(reason: string | null): boolean {
    attempted.value = reason
    return reason === null
  }
  return { line, allow }
}
