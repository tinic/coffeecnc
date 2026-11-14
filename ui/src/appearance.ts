import { ref, watch } from 'vue'

export const theme = ref<'light' | 'dark'>(new URLSearchParams(window.location.search).get('theme') === 'dark' ? 'dark' : 'light')

watch(theme, (value) => {
  document.documentElement.dataset['theme'] = value
}, { immediate: true })

export function toggleTheme(): void {
  theme.value = theme.value === 'light' ? 'dark' : 'light'
}
