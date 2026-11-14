import { afterEach, describe, expect, it, vi } from 'vitest'

import { createTransport } from '../src/bridge'
import type { FromMachine } from '../src/protocol'
import { createSimulator } from '../src/sim'
import { createPanel } from '../src/store'

vi.mock('../src/sim', () => ({ createSimulator: vi.fn(() => ({ simulated: true })) }))

afterEach(() => {
  vi.unstubAllGlobals()
  vi.clearAllMocks()
})

describe('panel transport', () => {
  it('keeps browser development on the simulator with its scene options', () => {
    vi.stubGlobal('window', { location: new URL('http://localhost:5174/?scene=paused') })
    expect(createTransport().simulated).toBe(true)
    expect(createSimulator).toHaveBeenCalledOnce()
    expect(vi.mocked(createSimulator).mock.calls[0]?.[0]?.get('scene')).toBe('paused')
  })

  it.each([undefined, { messageHandlers: { coffeecnc: {} } }])(
    'keeps a native page disconnected without a usable handler (%j)', (webkit) => {
      vi.stubGlobal('window', { location: new URL('coffeecnc://panel/index.html?scene=running'), webkit })
      const transport = createTransport()
      const panel = createPanel({ transport })
      const results: FromMachine[] = []
      transport.listen((message) => { results.push(message) })
      panel.send({ type: 'ready' })
      panel.send({ type: 'run' })
      panel.send({ type: 'abort' })
      expect(createSimulator).not.toHaveBeenCalled()
      expect(panel.state.simulated).toBe(false)
      expect(panel.state.hello).toBeNull()
      expect(panel.state.status).toBeNull()
      expect(panel.state.link).toBe('connecting')
      expect(panel.state.notice?.text).toContain('cannot connect to its native controller')
      expect(results).toEqual([1, 2, 3].map((id) => ({ type: 'result', id, outcome: 'rejected',
        reason: 'CoffeeCNC cannot connect to its native controller. Restart the application.' })))
    },
  )

  it('sends JSON to the native handler and delivers its replies', () => {
    const postMessage = vi.fn()
    vi.stubGlobal('window', { location: new URL('coffeecnc://panel/index.html'),
      webkit: { messageHandlers: { coffeecnc: { postMessage } } } })
    const transport = createTransport()
    const received: FromMachine[] = []
    transport.listen((message) => { received.push(message) })
    transport.send({ type: 'ready', id: 1 })
    expect(transport.simulated).toBe(false)
    expect(createSimulator).not.toHaveBeenCalled()
    expect(postMessage).toHaveBeenCalledWith('{"type":"ready","id":1}')
    const result = { type: 'result', id: 1, outcome: 'accepted', reason: '' } as const
    window.coffeecncReceive?.(result)
    expect(received).toEqual([result])
  })
})
