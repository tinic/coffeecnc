import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { nextTick } from 'vue'
import { panel } from '../src/machine'
import { cancelImport, importPart, part, placePart, removePart } from '../src/step'
import { importTimeoutMs, normalizeModel, type ImportReply } from '../src/step-model'
import { SimMachine } from '../src/sim/machine'

vi.mock('../src/machine', async () => {
  const { reactive, computed } = await import('vue')
  const state = reactive({ status: null, preview: null, link: 'live' })
  return { panel: state, live: computed(() => state.link === 'live') }
})

const workers: FakeWorker[] = []
class FakeWorker {
  onmessage: ((event: MessageEvent<ImportReply>) => void) | null = null
  onerror: (() => void) | null = null
  terminate = vi.fn()
  postMessage = vi.fn()
  constructor() { workers.push(this) }
  reply(reply: ImportReply): void { this.onmessage?.({ data: reply } as MessageEvent<ImportReply>) }
}

function file(read: () => Promise<ArrayBuffer> = () => Promise.resolve(new ArrayBuffer(3))): File {
  return { name: 'part.step', size: 3, arrayBuffer: read } as File
}
const geometry = normalizeModel({ success: true, meshes: [{ name: 'test', attributes: {
  position: { array: [0, 0, 0, 1, 0, 0, 0, 1, 0] },
}, index: { array: [0, 1, 2] } }] })

beforeEach(async () => {
  vi.useFakeTimers()
  vi.stubGlobal('Worker', FakeWorker)
  removePart()
  panel.status = new SimMachine({ emit: (): void => undefined }).status()
  panel.status.program.file = 'part.ngc'
  await nextTick()
  workers.length = 0
  part.error = ''
})
afterEach(() => { removePart(); vi.unstubAllGlobals(); vi.useRealTimers() })

describe('STEP import lifetime', () => {
  it('terminates the worker after success and retains the attachment across view changes', async () => {
    await importPart(file())
    workers[0]?.reply({ ok: true, geometry })
    expect(workers[0]?.terminate).toHaveBeenCalledOnce()
    expect(part.loading).toBe(false)
    expect(part.attachment?.name).toBe('part.step')
    panel.tab = 'manual'
    await nextTick()
    expect(part.attachment?.name).toBe('part.step')
    expect(placePart({ translation: [50, 30, 0], rotation: [0, 0, 90] })).toBe(true)
    expect(part.placed).toBe(true)
    expect(panel.status?.offsets.g5x).toEqual([150, 60, -160])
  })
  it('cancels before file reading finishes without starting a worker', async () => {
    let finish: (bytes: ArrayBuffer) => void = (): void => undefined
    const pending = importPart(file(() => new Promise((resolve) => { finish = resolve })))
    cancelImport()
    finish(new ArrayBuffer(3))
    await pending
    expect(workers).toHaveLength(0)
    expect(part.attachment).toBeNull()
  })
  it('ignores late replies after a program reload', async () => {
    await importPart(file())
    if (panel.status !== null) panel.status.program.revision++
    await nextTick()
    workers[0]?.reply({ ok: true, geometry })
    expect(part.attachment).toBeNull()
    expect(part.error).toContain('program changed')
    expect(workers[0]?.terminate).toHaveBeenCalledOnce()
  })
  it('keeps the previous model on an import failure, and rejects obsolete replies', async () => {
    await importPart(file())
    workers[0]?.reply({ ok: true, geometry })
    const previous = part.attachment
    await importPart(file())
    workers[1]?.reply({ ok: false, error: 'Bad STEP file' })
    workers[0]?.reply({ ok: true, geometry })
    expect(part.attachment).toBe(previous)
    expect(part.error).toBe('Bad STEP file')
  })
  it('terminates a hung import after the deadline', async () => {
    await importPart(file())
    vi.advanceTimersByTime(importTimeoutMs)
    expect(workers[0]?.terminate).toHaveBeenCalledOnce()
    expect(part.loading).toBe(false)
    expect(part.error).toContain('30 seconds')
    workers[0]?.reply({ ok: true, geometry })
    expect(part.attachment).toBeNull()
  })
})
