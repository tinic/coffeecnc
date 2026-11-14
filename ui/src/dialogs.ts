// How many dialogs are open. Keyboard jogging is off while any is.

import { ref } from 'vue'

export const openDialogs = ref<number>(0)

/**
 * Development: `?dialog=log|open|touchoff|tool` opens that dialog at once, so that a static
 * screenshot can show it.
 */
export const startDialog: string | null = new URLSearchParams(window.location.search).get('dialog')
