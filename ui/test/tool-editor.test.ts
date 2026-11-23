import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import ToolEditor from '../src/components/ToolEditor.vue'
import ToolPreviewDialog from '../src/components/ToolPreviewDialog.vue'
import { panel } from '../src/machine'
import type { FromMachine, ToMachine, Tool } from '../src/protocol'
import { hello, SimMachine } from '../src/sim/machine'
import { emptyToolModelDocument } from '../src/tool-model-settings'

const native = vi.hoisted((): { receive: (message: FromMachine) => void; sent: ToMachine[] } => ({ receive: () => undefined, sent: [] }))
// The tool model's WebGL chunk must not load in the background (#279).
vi.mock('../src/components/ToolModelCanvas.vue', () => import('./canvas-stub'))
vi.mock('../src/machine', async () => {
  const { computed } = await import('vue')
  const { createPanel } = await import('../src/store')
  const instance = createPanel({ transport: { simulated: false,
    listen: (listener): void => { native.receive = listener }, send: (command): void => { native.sent.push(command) },
  } })
  return { panel: instance.state, send: instance.send, onResult: instance.onResult,
    linkBlock: computed(() => instance.state.link === 'live' ? null : 'Connection unavailable') }
})
const tool: Tool = { number: 1, pocket: 1, length: 10, diameter: 6.35, comment: '' }
let wrapper: VueWrapper | null = null
function view(): VueWrapper { if (!wrapper) throw new Error('Missing editor'); return wrapper }
function open(value: Tool | null = tool): void {
  wrapper = mount(ToolEditor, { props: { tool: value }, global: { stubs: {
    ToolModelCanvas: true, ModalDialog: { template: '<div><slot/><slot name="footer"/></div>' },
  } } })
}
async function fill(label: string, value: string): Promise<void> {
  const field = view().findAll('label').find((item) => item.text().startsWith(label))
  if (!field) throw new Error(`Missing ${label}`)
  await field.get('input').setValue(value)
}
function last(): ToMachine { const command = native.sent.at(-1); if (!command) throw new Error('No command'); return command }
function reply(outcome: 'accepted' | 'failed' = 'accepted', reason = ''): void {
  native.receive({ type: 'result', id: last().id, outcome, reason })
}
async function save(): Promise<void> { await view().get('button.primary').trigger('click'); await flushPromises() }
beforeEach(() => {
  native.sent.length = 0
  native.receive({ ...hello, machine_id: 'G0759', tool_model_storage: 1 })
  native.receive({ type: 'tool_models', machine_id: 'G0759', revision: 0, availability: 'available', reason: '', document: emptyToolModelDocument() })
  native.receive(new SimMachine({ emit: (): void => undefined }).status())
  native.receive({ type: 'tool_table', revision: 1, tools: [tool] })
  panel.unit = 'mm'
})
afterEach(() => { wrapper?.unmount(); wrapper = null })

describe('combined tool and preview editor', () => {
  it('shows the live preview first, direct dimensions and read-only cutting diameter', () => {
    open()
    expect(view().get('.tool-editor').element.firstElementChild?.className).toBe('preview')
    expect(view().find('tool-model-canvas-stub').exists()).toBe(true)
    expect(view().findAll('.dimensions input').every((input) => input.attributes('disabled') === undefined)).toBe(true)
    expect(view().find('input[type="checkbox"]').exists()).toBe(false)
    expect(view().text()).toContain('6.35 mm · from Diameter above')
    expect(view().find('.diameter-source input').exists()).toBe(false)
  })
  it('shows the configured native probe number when adding a touch probe', async () => {
    native.receive({ ...hello, machine_id: 'Mill', touch_probe_tool: 37 })
    open(null)
    await view().get('[aria-label="Tool type"]').setValue('touch-probe')
    expect(view().get('.diameter-source').text()).toContain('Configured T37')
  })
  it('saves the uncommented G0759 T1 shape without rewriting tool details', async () => {
    open(); await save()
    expect(native.sent).toHaveLength(1)
    expect(last()).toMatchObject({ type: 'set_tool_model_assignment', tool: { number: 1, pocket: 1, diameter: 6.35, comment: '' }, family: 'end-mill' })
    expect(view().emitted('close')).toBeUndefined()
    const command = last(); if (command.type !== 'set_tool_model_assignment' || command.family === null) throw new Error('Missing assignment')
    const document = emptyToolModelDocument(); document.assignments.push({ tool: command.tool, family: command.family, overrides: command.overrides })
    native.receive({ type: 'tool_models', machine_id: 'G0759', revision: 1, availability: 'available', reason: '', document })
    reply(); await flushPromises(); expect(view().emitted('close')).toHaveLength(1)
  })
  it('saves the holder with the assignment and restores it in the editor and read-only preview', async () => {
    open()
    expect((view().get('[aria-label="Holder"]').element as HTMLSelectElement).value).toBe('')
    await view().get('[aria-label="Holder"]').setValue('tts-er20')
    await save()
    const command = last()
    if (command.type !== 'set_tool_model_assignment' || command.family === null || command.holder === undefined)
      throw new Error('Missing holder assignment')
    expect(native.sent).toHaveLength(1)
    expect(command.holder).toBe('tts-er20')
    const document = emptyToolModelDocument()
    document.assignments.push({ tool: command.tool, family: command.family, overrides: command.overrides, holder: command.holder })
    native.receive({ type: 'tool_models', machine_id: 'G0759', revision: 1, availability: 'available', reason: '', document })
    reply(); await flushPromises()
    expect(view().emitted('close')).toHaveLength(1)
    view().unmount(); open()
    expect((view().get('[aria-label="Holder"]').element as HTMLSelectElement).value).toBe('tts-er20')
    view().unmount()
    wrapper = mount(ToolPreviewDialog, { props: { tool }, global: { stubs: {
      ToolModelCanvas: true, ModalDialog: { template: '<div><slot/></div>' },
    } } })
    expect(view().get('dl').text()).toContain('HolderTTS / ER20')
  })
  it('waits for both tool acknowledgement and readback before saving the new shape identity', async () => {
    open(); await fill('Description', 'Quarter-inch end mill'); await save()
    const command = last(); if (command.type !== 'set_tool') throw new Error('Missing tool write')
    reply(); await flushPromises(); expect(native.sent).toHaveLength(1)
    native.receive({ type: 'tool_table', revision: 2, tools: [command.tool] }); await flushPromises()
    expect(native.sent).toHaveLength(2)
    expect(last()).toMatchObject({ type: 'set_tool_model_assignment', tool: { number: 1, pocket: 1, diameter: 6.35, comment: 'Quarter-inch end mill' } })
  })
  it('keeps a partial-save draft and retries only the preview after storage failure', async () => {
    open(); await fill('Length,', '7/2mm'); await save()
    const command = last(); if (command.type !== 'set_tool') throw new Error('Missing tool write')
    native.receive({ type: 'tool_table', revision: 2, tools: [command.tool] }); reply(); await flushPromises()
    reply('failed', 'Disk full'); await flushPromises()
    expect(view().text()).toContain('Tool details saved. Preview settings could not be saved. Disk full')
    expect(view().emitted('close')).toBeUndefined()
    await save(); expect(native.sent.filter((item) => item.type === 'set_tool')).toHaveLength(1)
    expect(last().type).toBe('set_tool_model_assignment'); reply('failed', 'Disk full'); await flushPromises()
  })
  it('does not save a shape after a refused table write or after closing during the write', async () => {
    open(); await fill('Description', 'Changed'); await save()
    reply('failed', 'Tool table is read-only'); await flushPromises()
    expect(view().text()).toContain('Tool table is read-only'); expect(native.sent).toHaveLength(1)
    await save(); const command = last(); if (command.type !== 'set_tool') throw new Error('Missing tool write')
    view().unmount(); wrapper = null
    native.receive({ type: 'tool_table', revision: 2, tools: [command.tool] }); reply(); await flushPromises()
    expect(native.sent).toHaveLength(2); expect(native.sent.every((item) => item.type === 'set_tool')).toBe(true)
  })
  it('updates preview diameter from a formula while preserving unedited precise dimensions', async () => {
    const precise = { ...tool, diameter: 6.350000001, length: 10.123456789 }
    native.receive({ type: 'tool_table', revision: 2, tools: [precise] }); panel.unit = 'inch'; open(precise)
    await fill('Description', 'Updated'); await save()
    expect(last()).toMatchObject({ type: 'set_tool', tool: { diameter: precise.diameter, length: precise.length } })
    reply('failed', 'Test refusal'); await flushPromises()
    await fill('Diameter,', '1/4in')
    expect(view().text()).toContain('0.25 inch · from Diameter above')
  })
  it('keeps tool numbers intact and proposes the next number when adding', () => {
    open(); expect(view().findAll('.tool-fields label').some((label) => label.text() === 'Tool number')).toBe(false)
    view().unmount(); open(null)
    expect((view().get('.tool-fields input').element as HTMLInputElement).value).toBe('2')
  })
})
