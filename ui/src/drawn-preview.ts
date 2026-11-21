import { computed, type ComputedRef } from 'vue'

import type { Preview } from './protocol'
import type { PanelState } from './store'

interface SavedPreview {
  file: string
  revision: number
  preview: Preview
}

/** Keep the last path while a replacement is pending for the same loaded program. */
export function useDrawnPreview(panel: Pick<PanelState, 'preview' | 'status'>): ComputedRef<Preview | null> {
  const saved = computed<SavedPreview | null>((previous) => {
    const status = panel.status
    if (!status?.program.file) return null
    const { file, revision } = status.program
    const preview = panel.preview
    if (preview?.context === status.preview_context && (preview.state === 'ready' || preview.state === 'failed')) {
      return { file, revision, preview }
    }
    return previous?.file === file && previous.revision === revision ? previous : null
  })
  return computed(() => saved.value?.preview ?? null)
}
