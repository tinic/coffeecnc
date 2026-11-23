import { computed, ref, watch } from 'vue'
import { decodeTheme, type PanelTheme } from './operator-settings'

export const themeOverride = decodeTheme(new URLSearchParams(window.location.search).get('theme'))
export const themePreference = ref<PanelTheme>(themeOverride ?? 'light')
export const themePreview = ref<PanelTheme | null>(null)
export const theme = computed(() => themePreview.value ?? themePreference.value)

watch(theme, (value) => {
  document.documentElement.dataset['theme'] = value
}, { immediate: true, flush: 'sync' })
