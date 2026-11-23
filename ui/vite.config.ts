import VueI18nPlugin from '@intlify/unplugin-vue-i18n/vite'
import vue from '@vitejs/plugin-vue'
import { defineConfig } from 'vitest/config'

import { buildStamp } from './build-stamp'

// `base: './'` because the coffeecnc executable serves the build from its own URI scheme.
export default defineConfig({
  base: './',
  // Panel text is compiled at build time; the page carries no message compiler. The stamp ties
  // dist to the sources it was built from; the native shell tests check it.
  plugins: [vue(), VueI18nPlugin({ include: [decodeURIComponent(new URL('./src/i18n/parts/**/*.json', import.meta.url).pathname)] }), buildStamp()],
  // The simulator imports the upstream samples from ../samples.
  server: { host: '127.0.0.1', port: 5174, strictPort: true, fs: { allow: ['..'] } },
  // Prebundle the lazy preview's studio lighting before the first dialog opens.
  optimizeDeps: { include: ['three', 'three/addons/environments/RoomEnvironment.js'] },
  build: { outDir: 'dist', emptyOutDir: true, chunkSizeWarningLimit: 600 },
  // The theme contrast test reads the design tokens from style.css; other CSS stays unprocessed.
  test: { environment: 'jsdom', include: ['test/**/*.test.ts'], setupFiles: ['test/setup-i18n.ts'], css: { include: [/src\/style\.css/] } },
})
