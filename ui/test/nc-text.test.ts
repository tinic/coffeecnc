import { afterEach, expect, it, vi } from 'vitest'
import { nextTick } from 'vue'
import { mount, type VueWrapper } from '@vue/test-utils'
import NcText from '../src/components/NcText.vue'

let wrapper: VueWrapper | null = null
const fontsDescriptor = Object.getOwnPropertyDescriptor(document, 'fonts')

afterEach(() => {
  wrapper?.unmount()
  wrapper = null
  vi.restoreAllMocks()
  vi.unstubAllGlobals()
  if (fontsDescriptor) Object.defineProperty(document, 'fonts', fontsDescriptor)
  else Reflect.deleteProperty(document, 'fonts')
})

it('keeps the full code width and bottom scroll position while following virtual rows', async () => {
  const header = `(${'long program header '.repeat(10)})`
  const lines = [header, ...Array<string>(98).fill('G1 X5 F500'), 'M30']
  const viewportHeight = 245
  const contentHeight = lines.length * 24
  vi.spyOn(Element.prototype, 'clientHeight', 'get').mockReturnValue(viewportHeight)
  vi.spyOn(Element.prototype, 'scrollHeight', 'get').mockReturnValue(contentHeight)
  const setTop = vi.spyOn(Element.prototype, 'scrollTop', 'set')
  Object.defineProperty(document, 'fonts', { configurable: true, value: { ready: Promise.resolve() } })
  const canvas = { measureText: (text: string): { width: number } => ({ width: text.length * 8 }) }
  vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue(canvas as CanvasRenderingContext2D)
  vi.stubGlobal('ResizeObserver', class {
    observe(): void { /* The test supplies fixed viewport dimensions. */ }
    disconnect(): void { /* No observer was scheduled. */ }
  })

  const pane = mount(NcText, { props: { lines, motionLine: 1, currentLine: 2, errorLine: 0 } })
  wrapper = pane
  await nextTick()
  const scroller = pane.get<HTMLElement>('.scroller')
  const spacer = pane.get<HTMLElement>('.spacer')
  const width = spacer.element.style.getPropertyValue('--code-width')
  expect(width).toBe(`${String(header.length * 8)}px`)
  expect(pane.get('.row:first-child .text').text()).toBe(header)

  await pane.setProps({ motionLine: 30 })
  expect(scroller.element.scrollTop).toBe(Math.round(29.5 * 24 - viewportHeight / 2))

  await pane.setProps({ motionLine: 99, currentLine: 100 })
  const bottom = contentHeight - viewportHeight
  expect(scroller.element.scrollTop).toBe(bottom)
  expect(pane.text()).not.toContain(header)
  expect(pane.get('.row:last-child .text').text()).toBe('M30')
  expect(spacer.element.style.getPropertyValue('--code-width')).toBe(width)

  setTop.mockClear()
  await pane.setProps({ motionLine: 0 })
  await pane.setProps({ motionLine: 99 })
  expect(scroller.element.scrollTop).toBe(bottom)
  expect(setTop).not.toHaveBeenCalled()
})

it('scrolls the last picked row above the line actions', async () => {
  const lines = [...Array<string>(39).fill('G1 X5 F500'), 'M30']
  const contentHeight = lines.length * 24
  const clientHeight = vi.spyOn(Element.prototype, 'clientHeight', 'get').mockReturnValue(245)
  vi.spyOn(Element.prototype, 'scrollHeight', 'get').mockReturnValue(contentHeight)
  vi.spyOn(HTMLCanvasElement.prototype, 'getContext').mockReturnValue(null)
  Object.defineProperty(document, 'fonts', { configurable: true, value: { ready: Promise.resolve() } })
  vi.stubGlobal('ResizeObserver', class {
    observe(): void { /* The viewport size is supplied by the test. */ }
    disconnect(): void { /* No observer was scheduled. */ }
  })

  const pane = mount(NcText, { props: { lines, motionLine: 0, currentLine: 0, errorLine: 0, restartable: true } })
  wrapper = pane
  const scroller = pane.get<HTMLElement>('.scroller')
  scroller.element.scrollTop = contentHeight - 245
  await scroller.trigger('scroll')

  clientHeight.mockReturnValue(185)
  await pane.get('.row:last-child .number').trigger('click')
  expect(pane.get('.row.picked .text').text()).toBe('M30')
  expect(pane.get('.line-actions').text()).toContain('Restart from line 40')
  expect(scroller.element.scrollTop + scroller.element.clientHeight).toBe(contentHeight)
})
