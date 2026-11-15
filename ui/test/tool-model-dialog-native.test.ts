import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import ToolModelDialog from '../src/components/ToolModelDialog.vue'
import type { FromMachine, ToMachine, Tool } from '../src/protocol'
import { hello, SimMachine } from '../src/sim/machine'
import { emptyToolModelDocument } from '../src/tool-model-settings'
import { toolModels } from '../src/tool-models'

const native = vi.hoisted((): { receive: (message: FromMachine) => void; sent: ToMachine[] } => ({ receive: () => undefined, sent: [] }))
vi.mock('../src/machine', async () => {
  const { createPanel } = await import('../src/store')
  const instance = createPanel({ transport: { simulated: false,
    listen: (listener): void => { native.receive = listener }, send: (command): void => { native.sent.push(command) },
  } })
  return { panel: instance.state, send: instance.send, onResult: instance.onResult }
})
const tool: Tool = { number: 3, pocket: 3, diameter: 6, length: 50, comment: 'End mill' }
let wrapper: VueWrapper | null = null
function open(value: Tool | null): VueWrapper {
  wrapper = mount(ToolModelDialog, { props: { tool: value }, global: { stubs: {
    ToolModelCanvas: true, ModalDialog: { template: '<div><slot/><slot name="footer"/></div>' },
  } } })
  return wrapper
}
function reply(outcome: 'accepted' | 'failed', reason = ''): void {
  const request = native.sent.at(-1)
  if (request === undefined) throw new Error('Expected a save request')
  native.receive({ type: 'result', id: request.id, outcome, reason })
}
beforeEach(() => {
  native.sent.length = 0
  native.receive({ ...hello, machine_id: 'G0759', tool_model_storage: 1 })
  native.receive({ type: 'tool_models', machine_id: 'G0759', revision: 0, availability: 'available', reason: '', document: emptyToolModelDocument() })
  native.receive(new SimMachine({ emit: (): void => undefined }).status())
  native.receive({ type: 'tool_table', revision: 1, tools: [tool] })
})
afterEach(() => { wrapper?.unmount(); vi.restoreAllMocks() })

describe('native tool model editor', () => {
  it('keeps a failed draft and only closes after the updated snapshot and save confirmation', async () => {
    const w = open(null)
    expect(w.text()).toContain('Save keeps these models for machine G0759')
    await w.findAll('input')[4]?.setValue('40')
    await w.get('button.primary').trigger('click')
    expect(w.get('button.primary').text()).toBe('Saving…')
    expect(w.emitted('close')).toBeUndefined()
    expect(toolModels.defaults['end-mill'].stickout).toBe(35)
    reply('failed', 'Disk full')
    await flushPromises()
    expect(w.text()).toContain('Disk full')
    expect((w.findAll('input')[4]?.element as HTMLInputElement).value).toBe('40')
    expect(w.emitted('close')).toBeUndefined()
    await w.get('button.primary').trigger('click')
    const document = emptyToolModelDocument(); document.defaults['end-mill'].stickout = 40
    native.receive({ type: 'tool_models', machine_id: 'G0759', revision: 1, availability: 'available', reason: '', document })
    await flushPromises()
    expect(w.emitted('close')).toBeUndefined()
    reply('accepted')
    await flushPromises()
    expect(w.emitted('close')).toHaveLength(1)
    expect(toolModels.defaults['end-mill'].stickout).toBe(40)
  })

  it('keeps a draft but disables saving if the saved revision or machine changes', async () => {
    const w = open(null)
    await w.findAll('input')[4]?.setValue('40')
    native.receive({ type: 'tool_models', machine_id: 'G0759', revision: 1, availability: 'available', reason: '', document: emptyToolModelDocument() })
    await flushPromises()
    expect(w.text()).toContain('Saved settings changed')
    expect(w.get('button.primary').attributes('disabled')).toBeDefined()
    expect((w.findAll('input')[4]?.element as HTMLInputElement).value).toBe('40')
    native.receive({ ...hello, machine_id: 'AnotherMill', tool_model_storage: 1 })
    await flushPromises()
    expect(w.text()).toContain('machine connection changed')
    expect(w.emitted('close')).toBeUndefined()
    expect(native.sent).toEqual([])
  })

  it('shows unavailable storage and prevents Reset family from overwriting a corrupt document', async () => {
    native.receive({ type: 'tool_models', machine_id: 'G0759', revision: 0, availability: 'unavailable', reason: 'Settings file is corrupt', document: null })
    const w = open(null)
    await w.findAll('button').find((button) => button.text() === 'Reset this family')?.trigger('click')
    expect(w.text()).toContain('Settings file is corrupt')
    expect(w.get('button.primary').attributes('disabled')).toBeDefined()
    expect(native.sent).toEqual([])
  })
})
