// Stands in for a lazy WebGL canvas (ProbeGuideCanvas, ToolModelCanvas) in tests that only open
// its dialog. A `stubs` entry does not stop defineAsyncComponent from loading the real chunk;
// that import (three.js, appearance.ts) can finish after the jsdom environment is gone and then
// throws "window is not defined" (#279). Mock the module with this instead:
//   vi.mock('../src/components/ProbeGuideCanvas.vue', () => import('./canvas-stub'))
import { defineComponent, h, type VNode } from 'vue'

export default defineComponent({
  name: 'CanvasStub',
  inheritAttrs: false,
  setup(_props, { expose }) {
    expose({ seek: (): void => undefined })
    return (): VNode => h('div', { class: 'canvas-stub' })
  },
})
