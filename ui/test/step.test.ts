import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { nextTick } from 'vue'
import { panel } from '../src/machine'
import { cancelImport, importPart, loadProgramPart, part, placePart, programPartUrl, removePart } from '../src/step'
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

describe('the model beside the program', () => {
  it('asks the native panel for the STEP file of the loaded program and imports it', async () => {
    expect(programPartUrl('/home/cnc/my part.ngc')).toBe('coffeecnc://panel/program-part?program=%2Fhome%2Fcnc%2Fmy%20part.ngc')
    if (panel.status === null) throw new Error('Missing status')
    panel.status.program.file = '/home/cnc/plate.ngc'
    const fetcher = vi.fn(() => Promise.resolve({ ok: true, arrayBuffer: () => Promise.resolve(new ArrayBuffer(3)) } as Response))
    await loadProgramPart(fetcher)
    expect(fetcher).toHaveBeenCalledWith('coffeecnc://panel/program-part?program=%2Fhome%2Fcnc%2Fplate.ngc')
    await vi.waitFor(() => { expect(workers).toHaveLength(1) })
    workers[0]?.reply({ ok: true, geometry })
    await vi.waitFor(() => { expect(part.attachment?.name).toBe('plate.step') })
    expect(part.attachment?.note).toBe('Loaded with the program')
    // It goes with its program: changing the program reports nothing.
    panel.status.program.file = '/home/cnc/other.ngc'
    await nextTick()
    expect(part.attachment).toBeNull()
    expect(part.error).toBe('')
  })

  it('does nothing without a model, in the simulator, or over a model attached meanwhile', async () => {
    if (panel.status === null) throw new Error('Missing status')
    panel.status.program.file = '/home/cnc/lone.ngc'
    await loadProgramPart(vi.fn(() => Promise.resolve({ ok: false } as Response)))
    expect(part.attachment).toBeNull()
    expect(part.error).toBe('')
    const simulated = vi.fn()
    ;(panel as { simulated?: boolean }).simulated = true
    await loadProgramPart(simulated)
    expect(simulated).not.toHaveBeenCalled()
    ;(panel as { simulated?: boolean }).simulated = false
  })

  it('asks the LAN server for the current program model and drops an obsolete download', async () => {
    if (panel.status === null) throw new Error('Missing status')
    const before = window.location.pathname
    window.history.replaceState(null, '', '/view')
    try {
      panel.status.program.file = '/home/cnc/my part.ngc'
      panel.status.program.revision = 7
      let finish: (response: Response) => void = () => undefined
      const request = vi.fn(() => new Promise<Response>((resolve) => { finish = resolve }))
      const pending = loadProgramPart(request)
      expect(request).toHaveBeenCalledWith('/program-part?revision=7&program=%2Fhome%2Fcnc%2Fmy%20part.ngc')
      panel.status.program.revision++
      finish({ ok: true, arrayBuffer: () => Promise.resolve(new ArrayBuffer(3)) } as Response)
      await pending
      expect(workers).toHaveLength(0)
    } finally {
      window.history.replaceState(null, '', before)
    }
  })
})

