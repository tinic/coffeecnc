import vue from '@vitejs/plugin-vue'
import { defineConfig } from 'vitest/config'

// `base: './'` because the coffeecnc executable serves the build from its own URI scheme.
export default defineConfig({
  base: './',
  plugins: [vue()],
  // The simulator imports the upstream samples from ../samples.
  server: { host: '127.0.0.1', port: 5174, strictPort: true, fs: { allow: ['..'] } },
  build: { outDir: 'dist', emptyOutDir: true, chunkSizeWarningLimit: 600 },
  test: { environment: 'jsdom', include: ['test/**/*.test.ts'] },
})
