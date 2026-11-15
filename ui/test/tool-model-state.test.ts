import { afterEach, describe, expect, it, vi, type Mock } from 'vitest'
import type { Listener } from '../src/bridge'
import type { FromMachine, Hello, ToMachine, Tool } from '../src/protocol'
import { hello, SimMachine } from '../src/sim/machine'
import { answerTimeoutMs, createPanel } from '../src/store'
import { factoryToolDefaults } from '../src/tool-model'
import { emptyToolModelDocument, type ToolModelDocument } from '../src/tool-model-settings'
import { createToolModelState } from '../src/tool-model-state'

const tool: Tool = { number: 3, pocket: 7, diameter: 6.35, length: 50, comment: 'Quarter inch mill' }
const disposals: (() => void)[] = []
afterEach(() => { for (const dispose of disposals.splice(0)) dispose() })

interface Rig {
  panel: ReturnType<typeof createPanel>
  models: ReturnType<typeof createToolModelState>
  sent: ToMachine[]
  storage: { getItem: Mock<() => string | null>; setItem: Mock<(key: string, value: string) => void> }
  storageAccess: Mock<() => Pick<Storage, 'getItem' | 'setItem'>>
  receive: (value: FromMachine) => void
  connect: (id?: string | null, capability?: boolean) => void
  snapshot: (revision?: number, document?: ToolModelDocument, id?: string) => void
  table: (tools?: Tool[]) => void
  reply: (outcome?: 'accepted' | 'failed' | 'rejected', reason?: string) => void
  pass: (ms: number) => void
  refuseSynchronously: (reason: string) => void
}
function rig(simulated = false): Rig {
  let listener: Listener = () => undefined
  let timer: (() => void) | null = null
  let now = 0
  let synchronousReason: string | null = null
  const sent: ToMachine[] = []
  const storage = { getItem: vi.fn<() => string | null>(() => null), setItem: vi.fn<(key: string, value: string) => void>() }
  const storageAccess = vi.fn(() => storage)
  const panel = createPanel({ transport: {
    simulated, listen: (value): void => { listener = value }, send: (command): void => {
      sent.push(command)
      if (synchronousReason !== null) listener({ type: 'result', id: command.id, outcome: 'rejected', reason: synchronousReason })
    },
  }, now: (): number => now, timers: {
    setInterval: (callback): number => { timer = callback; return 1 }, clearInterval: (): void => { timer = null },
  } })
  panel.start()
  const models = createToolModelState(panel.state, panel, storageAccess)
  disposals.push(models.dispose, panel.stop)
  const receive = (value: FromMachine): void => { listener(value) }
  const connect = (id: string | null = 'G0759', capability = true): void => {
    const greeting: Hello = { ...hello, machine_id: id }
    if (capability) greeting.tool_model_storage = 1
    receive(greeting)
  }
  const snapshot = (revision = 0, document = emptyToolModelDocument(), id = 'G0759'): void => {
    receive({ type: 'tool_models', machine_id: id, revision, availability: 'available', reason: '', document })
  }
  const table = (tools = [tool]): void => {
    receive(new SimMachine({ emit: (): void => undefined }).status())
    receive({ type: 'tool_table', revision: 1, tools })
  }
  const reply = (outcome: 'accepted' | 'failed' | 'rejected' = 'accepted', reason = ''): void => {
    const command = sent.at(-1)
    if (command === undefined) throw new Error('Expected a sent command')
    receive({ type: 'result', id: command.id, outcome, reason })
  }
  return { panel, models, sent, storage, storageAccess, receive, connect, snapshot, table, reply,
    pass: (ms: number): void => { now += ms; timer?.() },
    refuseSynchronously: (reason): void => { synchronousReason = reason } }
}

function saved(): ToolModelDocument {
  return { ...emptyToolModelDocument(), assignments: [{
    tool: { number: tool.number, pocket: tool.pocket, diameter: tool.diameter, comment: tool.comment },
    family: 'end-mill', overrides: {},
  }] }
}

describe('tool model persistence state', () => {
  it('never reads or writes browser storage from a native transport, even for a simulated machine', async () => {
    const r = rig()
    r.connect('G0759', false)
    expect(r.panel.state.hello?.simulated).toBe(true)
    expect(r.models.state.storage).toBe('session_only')
    const defaults = factoryToolDefaults(); defaults.drill.stickout = 80
    expect((await r.models.saveDefaults(defaults, r.models.context())).saved).toBe(true)
    r.table()
    expect((await r.models.saveAssignment(tool, 'end-mill', {}, r.models.context())).saved).toBe(true)
    expect(r.sent).toEqual([])
    expect(r.storageAccess).not.toHaveBeenCalled()
    expect(r.models.state.saved).toContain('page session')
  })

  it('keeps browser defaults separate and reports a browser write failure', async () => {
    const r = rig(true)
    expect(r.storage.getItem).toHaveBeenCalledOnce()
    const defaults = factoryToolDefaults(); defaults.drill.stickout = 80
    expect((await r.models.saveDefaults(defaults, r.models.context())).saved).toBe(true)
    expect(r.storage.setItem).toHaveBeenCalledOnce()
    r.storage.setItem.mockImplementation(() => { throw new Error('quota') })
    expect((await r.models.saveDefaults(defaults, r.models.context())).saved).toBe(false)
    expect(r.models.state.storageError).toContain('until this page closes')
    expect(r.sent).toEqual([])
  })

  it('treats loading and unavailable storage as unavailable, not an empty document to overwrite', async () => {
    const r = rig(); r.connect()
    expect((await r.models.saveDefaults(factoryToolDefaults(), r.models.context())).saved).toBe(false)
    for (const reason of ['Unsupported file version', 'Settings file is corrupt', 'Already in use']) {
      r.receive({ type: 'tool_models', machine_id: 'G0759', revision: 0, availability: 'unavailable', reason, document: null })
      expect((await r.models.saveDefaults(factoryToolDefaults(), r.models.context())).reason).toBe(reason)
    }
    r.connect(null)
    r.receive({ type: 'tool_models', machine_id: null, revision: 0, availability: 'session_only', reason: 'No identity', document: null })
    expect((await r.models.saveDefaults(factoryToolDefaults(), r.models.context())).saved).toBe(true)
    expect(r.sent).toEqual([])
    expect(r.storageAccess).not.toHaveBeenCalled()
  })

  it('restores only matching live rows and preserves the saved catalog across stale or deleted tables', () => {
    const r = rig(); r.connect(); r.snapshot(2, saved())
    expect(r.models.state.assignments.size).toBe(0)
    r.table(); expect(r.models.state.assignments.has(3)).toBe(true)
    r.table([{ ...tool, length: 999 }]); expect(r.models.state.assignments.has(3)).toBe(true)
    r.table([{ ...tool, diameter: 8 }]); expect(r.models.state.assignments.size).toBe(0)
    r.table([]); expect(r.panel.state.toolModels?.document?.assignments).toHaveLength(1)
    r.table(); r.pass(1200); expect(r.models.state.assignments.size).toBe(0)
    r.table(); expect(r.models.state.assignments.has(3)).toBe(true)
    r.receive({ ...hello, machine_id: 'OtherMill', tool_model_storage: 1 })
    expect(r.panel.state.toolTable).toBeNull()
    expect(r.models.state.assignments.size).toBe(0)
  })

  it('waits for a new snapshot AND accepted result, and keeps unmatched saved entries out of the request', async () => {
    const r = rig(); r.connect(); r.snapshot(2, saved())
    const edit = r.models.context()
    const defaults = factoryToolDefaults(); defaults.drill.stickout = 80
    const saving = r.models.saveDefaults(defaults, edit)
    expect(r.models.state.saving).toBe(true)
    expect(r.models.state.defaults.drill.stickout).toBe(75)
    expect(r.sent[0]).toEqual({ type: 'set_tool_model_defaults', id: 1, machine_id: 'G0759', base_revision: 2, defaults })
    expect(JSON.stringify(r.sent[0]).length).toBeLessThan(16384)
    r.snapshot(3, { ...saved(), defaults })
    expect(r.models.state.saving).toBe(true)
    expect(r.models.state.saved).toBe('')
    r.reply()
    expect((await saving).saved).toBe(true)
    expect(r.models.state.saved).toContain('G0759')
    expect(r.panel.state.toolModels?.document?.assignments).toHaveLength(1)
    expect((await r.models.saveDefaults(factoryToolDefaults(), edit)).saved).toBe(false)
    expect(r.sent).toHaveLength(1)
  })

  it('saves and clears one exact-mm binding without sending or replacing the catalog', async () => {
    const r = rig(); r.connect(); r.snapshot(); r.table()
    const saving = r.models.saveAssignment(tool, 'end-mill', { stickout: 30 }, r.models.context())
    expect(r.sent[0]).toMatchObject({ type: 'set_tool_model_assignment', machine_id: 'G0759', base_revision: 0,
      tool: { number: 3, pocket: 7, diameter: 6.35, comment: tool.comment }, family: 'end-mill', overrides: { stickout: 30 } })
    expect(r.sent[0]).not.toHaveProperty('tool.length')
    expect(r.models.state.assignments.size).toBe(0)
    r.snapshot(1, saved()); r.reply(); expect((await saving).saved).toBe(true)
    const clearing = r.models.saveAssignment(tool, null, {}, r.models.context())
    expect(r.sent[1]).toMatchObject({ type: 'set_tool_model_assignment', family: null, overrides: {}, base_revision: 1 })
    r.snapshot(2); r.reply(); expect((await clearing).saved).toBe(true)
    expect(r.models.state.assignments.size).toBe(0)
  })

  it('retains confirmed values after a rejected save, including a synchronous local refusal', async () => {
    const r = rig(); r.connect(); r.snapshot(2)
    const defaults = factoryToolDefaults(); defaults.drill.stickout = 80
    const saving = r.models.saveDefaults(defaults, r.models.context())
    r.reply('failed', 'Disk full')
    expect(await saving).toEqual({ saved: false, reason: 'Disk full' })
    expect(r.models.state.defaults.drill.stickout).toBe(75)
    expect(r.models.state.saving).toBe(false)
    // A missing native bridge can reject inside send, before it returns a command id.
    r.refuseSynchronously('No native bridge')
    expect(await r.models.saveDefaults(defaults, r.models.context())).toEqual({ saved: false, reason: 'No native bridge' })
    expect(r.models.state.saving).toBe(false)
  })

  it('does not call an accepted result saved without a newer snapshot', async () => {
    const r = rig(); r.connect(); r.snapshot(2)
    const saving = r.models.saveDefaults(factoryToolDefaults(), r.models.context())
    r.reply()
    expect((await saving).saved).toBe(false)
    expect(r.models.state.uncertain).toBe(true)
    expect(r.models.state.saved).toBe('')
  })

  it('does not treat timeout as cancellation or a late snapshot as save confirmation', async () => {
    const r = rig(); r.connect(); r.snapshot(2)
    const saving = r.models.saveDefaults(factoryToolDefaults(), r.models.context())
    r.pass(answerTimeoutMs + 200)
    expect((await saving).reason).toContain('may still finish')
    expect((await r.models.saveDefaults(factoryToolDefaults(), r.models.context())).saved).toBe(false)
    expect(r.sent).toHaveLength(1)
    r.snapshot(3); r.reply()
    expect(r.models.state.uncertain).toBe(false)
    expect(r.models.state.saved).toBe('')
  })

  it('invalidates a pending save on machine change and ignores old-machine snapshots/results', async () => {
    const r = rig(); r.connect(); r.snapshot(2)
    const saving = r.models.saveDefaults(factoryToolDefaults(), r.models.context())
    r.connect('OtherMill')
    expect((await saving).saved).toBe(false)
    r.snapshot(3); r.reply()
    expect(r.models.state.revision).toBeNull()
    expect(r.models.state.saved).toBe('')
    r.snapshot(0, emptyToolModelDocument(), 'OtherMill')
    expect(r.models.state.storage).toBe('available')
    expect(r.models.state.revision).toBe(0)
  })
})
