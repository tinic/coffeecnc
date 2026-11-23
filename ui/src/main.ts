import { createApp, watch } from 'vue'

import App from './App.vue'
import { languageReady, theme, themeOverride } from './appearance'
import { i18n } from './i18n'
import { operatorSettings, panel, start } from './machine'
import { preventPageZoom } from './page-zoom'
import './style.css'

preventPageZoom()
const app = createApp(App)
app.use(i18n)
if (!panel.simulated && themeOverride === null) {
  // Keep the native panel empty until its per-machine theme and language arrive. Limit the wait
  // so an unavailable settings service never prevents the operator from using the panel.
  document.documentElement.dataset['theme'] = 'dark'
  let mounted = false
  function mount(): void {
    if (mounted) return
    mounted = true
    stop()
    window.clearTimeout(deadline)
    document.documentElement.dataset['theme'] = theme.value
    app.mount('#app')
  }
  const stop = watch(() => operatorSettings.state.storage, (storage) => {
    // The language's text is a local chunk; it loads well within the deadline.
    if (storage !== 'loading') void languageReady.finally(mount)
  })
  const deadline = window.setTimeout(mount, 300)
  start()
} else {
  void languageReady.finally(() => {
    app.mount('#app')
    start()
  })
}
