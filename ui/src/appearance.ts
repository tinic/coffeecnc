import { ref, watch } from 'vue'
import { decodeTheme, type PanelTheme } from './operator-settings'

export const themeOverride = decodeTheme(new URLSearchParams(window.location.search).get('theme'))
export const theme = ref<PanelTheme>(themeOverride ?? 'light')

watch(theme, (value) => {
  document.documentElement.dataset['theme'] = value
}, { immediate: true, flush: 'sync' })
