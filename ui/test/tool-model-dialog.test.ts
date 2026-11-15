import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { mount, type VueWrapper } from '@vue/test-utils'
import { nextTick } from 'vue'
import ToolModelDialog from '../src/components/ToolModelDialog.vue'
import { panel } from '../src/machine'
import type { Tool } from '../src/protocol'
import { factoryToolDefaults } from '../src/tool-model'
import { toolModels } from '../src/tool-models'

vi.mock('../src/machine', async () => {
  const { reactive } = await import('vue')
  return { panel: reactive({ simulated: true, unit: 'mm', toolTable: null, hello: null, toolModels: null }), send: vi.fn(), onResult: (): (() => void) => () => undefined }
})
const tool: Tool = { number: 1, pocket: 1, diameter: 6, length: -123, comment: 'End mill' }
let wrapper: VueWrapper | null = null
function open(value: Tool | null): VueWrapper {
  wrapper = mount(ToolModelDialog, { props: { tool: value }, global: { stubs: {
    ToolModelCanvas: true, ModalDialog: { template: '<div><slot/><slot name="footer"/></div>' },
  } } })
  return wrapper
}
beforeEach(async () => {
  panel.unit = 'mm'; panel.toolTable = { type: 'tool_table', revision: 1, tools: [tool] }
  toolModels.defaults = factoryToolDefaults(); toolModels.assignments.clear(); localStorage.clear()
  await nextTick()
})
afterEach(() => { wrapper?.unmount(); vi.restoreAllMocks() })

describe('tool model editor', () => {
  it('retains edits across families and saves browser defaults only on Save', async () => {
    const w = open(null)
    const inputs = w.findAll('input')
    await inputs[4]?.setValue('40') // stick-out
    await w.get('select').setValue('drill')
    await w.get('select').setValue('end-mill')
    expect((w.findAll('input')[4]?.element as HTMLInputElement).value).toBe('40')
    expect(toolModels.defaults['end-mill'].stickout).toBe(35)
    await w.findAll('button').find((b) => b.text() === 'Save defaults')?.trigger('click')
    expect(toolModels.defaults['end-mill'].stickout).toBe(40)
    expect(localStorage.getItem('coffeecnc.tool-model-defaults.v1')).toContain('"stickout":40')
  })
  it('converts inch overrides to mm and invalidates a changed tool, not a touched-off tool', async () => {
    panel.unit = 'inch'
    const w = open(tool)
    await w.get('input[type="checkbox"]').setValue(true)
    const inputs = w.findAll('input[type="text"]')
    await inputs[4]?.setValue('1')
    await w.findAll('button').find((b) => b.text() === 'Save preview model')?.trigger('click')
    expect(toolModels.assignments.get(1)?.overrides.stickout).toBe(25.4)
    panel.toolTable = { type: 'tool_table', revision: 2, tools: [{ ...tool, length: 50 }] }
    await nextTick()
    expect(toolModels.assignments.has(1)).toBe(true)
    panel.toolTable = { type: 'tool_table', revision: 3, tools: [{ ...tool, diameter: 8 }] }
    await nextTick()
    expect(toolModels.assignments.has(1)).toBe(false)
    expect(w.text()).toContain('The tool table changed')
    expect(w.findAll('button').find((b) => b.text() === 'Save preview model')?.attributes('disabled')).toBeDefined()
  })
  it('does not save invalid geometry or apply canceled edits', async () => {
    const w = open(null)
    await w.findAll('input')[4]?.setValue('100')
    expect(w.text()).toContain('Cutting length must fit')
    expect(w.findAll('button').find((b) => b.text() === 'Save defaults')?.attributes('disabled')).toBeDefined()
    await w.findAll('button').find((b) => b.text() === 'Cancel')?.trigger('click')
    expect(toolModels.defaults).toEqual(factoryToolDefaults())
    expect(localStorage.length).toBe(0)
  })
})
