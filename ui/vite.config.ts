import vue from '@vitejs/plugin-vue'
import { defineConfig } from 'vitest/config'

import { buildStamp } from './build-stamp'

// `base: './'` because the coffeecnc executable serves the build from its own URI scheme.
export default defineConfig({
  base: './',
  // The stamp ties dist to the sources it was built from; the native shell tests check it.
  plugins: [vue(), buildStamp()],
  // The simulator imports the upstream samples from ../samples.
  server: { host: '127.0.0.1', port: 5174, strictPort: true, fs: { allow: ['..'] } },
  // Prebundle the lazy preview's studio lighting before the first dialog opens.
  optimizeDeps: { include: ['three', 'three/addons/environments/RoomEnvironment.js'] },
  build: { outDir: 'dist', emptyOutDir: true, chunkSizeWarningLimit: 600 },
  test: { environment: 'jsdom', include: ['test/**/*.test.ts'] },
})
