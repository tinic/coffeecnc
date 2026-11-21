import { createApp, watch } from 'vue'

import App from './App.vue'
import { theme, themeOverride } from './appearance'
import { operatorSettings, panel, start } from './machine'
import { preventPageZoom } from './page-zoom'
import './style.css'

preventPageZoom()
const app = createApp(App)
if (!panel.simulated && themeOverride === null) {
  // Keep the native panel empty until its per-machine theme arrives. Limit the wait so an
  // unavailable settings service never prevents the operator from using the panel.
  document.documentElement.dataset['theme'] = 'dark'
  function mount(): void {
    stop()
    window.clearTimeout(deadline)
    document.documentElement.dataset['theme'] = theme.value
    app.mount('#app')
  }
  const stop = watch(() => operatorSettings.state.storage, (storage) => {
    if (storage !== 'loading') mount()
  })
  const deadline = window.setTimeout(mount, 300)
  start()
} else {
  app.mount('#app')
  start()
}
