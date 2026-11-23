import { computed, ref, watch } from 'vue'
import { decodeLanguage, setLanguage, type Language } from './i18n'
import { decodeTheme, type PanelTheme } from './operator-settings'

export const themeOverride = decodeTheme(new URLSearchParams(window.location.search).get('theme'))
export const themePreference = ref<PanelTheme>(themeOverride ?? 'light')
export const themePreview = ref<PanelTheme | null>(null)
export const theme = computed(() => themePreview.value ?? themePreference.value)

watch(theme, (value) => {
  document.documentElement.dataset['theme'] = value
}, { immediate: true, flush: 'sync' })

// `?lang=de` overrides the saved language, as `?theme=` does the theme.
export const languageOverride = decodeLanguage(new URLSearchParams(window.location.search).get('lang'))
export const languagePreference = ref<Language>(languageOverride ?? 'en')
/** The language Settings shows while the operator chooses; Cancel restores the saved one. */
export const languagePreview = ref<Language | null>(null)
export const panelLanguage = computed(() => languagePreview.value ?? languagePreference.value)
/** Settles when the panel shows the language last chosen. */
export let languageReady: Promise<void> = setLanguage(panelLanguage.value)
watch(panelLanguage, (value) => { languageReady = setLanguage(value) }, { flush: 'sync' })
