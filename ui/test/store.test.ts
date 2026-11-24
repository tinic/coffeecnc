import { describe, expect, it } from 'vitest'
import { watch } from 'vue'

import type { Listener, Transport } from '../src/bridge'
import type { FromMachine, Status, ToMachine } from '../src/protocol'
import { hello, SimMachine } from '../src/sim/machine'
import { answerTimeoutMs, createPanel, describeCommand, restartTimeoutMs, staleAfterMs, type Panel } from '../src/store'
import { emptyToolModelDocument } from '../src/tool-model-settings'
import { heightMapAbortMs, probeAnswerMs } from '../src/height-map'

interface Rig {
  panel: Panel
  sent: ToMachine[]
  /** Delivers a message from the machine. */
  receive: (message: FromMachine) => void
  /** Advances the clock and runs the panel's watchdog. */
  pass: (ms: number) => void
  status: (change?: Partial<Status>) => Status
}

function rig(): Rig {
  const sent: ToMachine[] = []
  const listeners: Listener[] = []
  const transport: Transport = {
    simulated: false,
    send: (message): void => {
      sent.push(message)
    },
    listen: (listener): void => {
      listeners.push(listener)
    },
  }
  let now = 1_000_000
  let watchdog: (() => void) | null = null
  const panel = createPanel({
    transport,
    now: (): number => now,
    timers: {
      setInterval: (callback): number => {
        watchdog = callback
        return 1
      },
      clearInterval: (): void => {
        watchdog = null
      },
    },
  })
  panel.start()
  const template = new SimMachine({ emit: (): void => undefined }).status()
  return {
    panel,
    sent,
    receive: (message): void => {
      for (const listener of listeners) {
        listener(message)
      }
    },
    pass: (ms): void => {
      now += ms
      watchdog?.()
    },
    status: (change = {}): Status => ({ ...template, ...change }),
  }
}

describe('commands', () => {
  it('signals motion cancellation before sending a panel stop to the transport', () => {
    const { panel, sent } = rig()
    const stopAt: number[] = []
    const stopWatch = watch(() => panel.state.motionStopRequest, () => {
      stopAt.push(sent.length)
    }, { flush: 'sync' })
    panel.send({ type: 'mdi', text: 'G0 X10' })
    panel.send({ type: 'abort' })
    panel.send({ type: 'set_machine', state: 'estop' })
    panel.send({ type: 'set_machine', state: 'off' })
    panel.send({ type: 'set_machine', state: 'on' })
    stopWatch()
    expect(stopAt).toEqual([1, 2, 3])
  })
  it('keeps probe command failures in the log and shows other machine messages globally', () => {
    const { panel, receive, status } = rig()
    receive(hello)
    receive(status({ link: 'live', machine: 'on' }))
    panel.state.tab = 'probe'
    const id = panel.send({ type: 'fixed_setter', action: 'material', direction: 'x_plus',
      tool: 99, x: 0, y: 0, z: 0 })
    receive({ type: 'result', id, outcome: 'failed', reason: 'No probe contact' })
    expect(panel.state.log.at(-1)?.text).toBe('No probe contact')
    expect(panel.state.notice).toBeNull()
    receive({ type: 'message', level: 'info', text: 'Jog stopped: probe contact', time: 1 })
    expect(panel.state.notice?.text).toBe('Jog stopped: probe contact')
  })

  it('binds tool settings to the current capable hello, withholds invalid data and never regresses revisions', () => {
    const { panel, receive } = rig()
    const snapshot = { type: 'tool_models', machine_id: 'G0759', revision: 2,
      availability: 'available', reason: '', document: emptyToolModelDocument() } as const
    receive(snapshot)
    receive(hello)
    receive(snapshot)
    expect(panel.state.toolModels).toBeNull()
    receive({ ...hello, machine_id: 'G0759', tool_model_storage: 1 })
    receive(snapshot)
    expect(panel.state.toolModels).toEqual(snapshot)
    receive({ ...snapshot, revision: 1 })
    receive({ ...snapshot, machine_id: 'AnotherMill', revision: 3 })
    expect(panel.state.toolModels?.revision).toBe(2)
    receive({ ...snapshot, revision: NaN })
    expect(panel.state.toolModels?.availability).toBe('unavailable')
    expect(panel.state.notice?.text).toContain('Invalid tool model settings')
    receive({ ...hello, machine_id: 'AnotherMill', tool_model_storage: 1 })
    expect(panel.state.toolModels).toBeNull()
    receive(snapshot)
    expect(panel.state.toolModels).toBeNull()
    receive({ ...snapshot, machine_id: 'AnotherMill' })
    receive({ ...hello, protocol: hello.protocol + 1 })
    expect(panel.state.toolModels).toBeNull()
  })
  it('does not present incompatible status as live or forward new motion', () => {
    const { panel, sent, receive, status, pass } = rig()
    receive(hello)
    receive(status({ link: 'live' }))
    receive({ ...hello, protocol: hello.protocol + 1 })
    receive(status({ link: 'live' }))
    expect(panel.state.link).toBe('incompatible')
    pass(2000)
    expect(panel.state.link).toBe('incompatible')
    expect(panel.state.status).toBeNull()
    const id = panel.send({ type: 'mdi', text: 'G0 X10' })
    expect(sent).toEqual([])
    expect(panel.state.mdi.find((entry) => entry.id === id)?.outcome).toBe('rejected')
    expect(panel.state.notice?.text).toContain('Protocol mismatch')
  })

  it('retains stop and reconnect attempts during a mismatch without claiming acceptance', () => {
    const { panel, sent, receive, pass } = rig()
    receive({ ...hello, protocol: hello.protocol + 1 })
    const attempts = [
      { type: 'ready' }, { type: 'abort' }, { type: 'jog_stop_all' },
      { type: 'jog', axis: 'z', direction: 1, velocity: 0, held: false },
      { type: 'set_machine', state: 'estop' },
    ] satisfies import('../src/protocol').CommandBody[]
    for (const command of attempts) panel.send(command)
    expect(sent).toEqual(attempts.map((command, index) => ({ ...command, id: index + 1 })))
    panel.send({ type: 'set_machine', state: 'off' }) // This could reset E-stop; it is not a stop attempt.
    panel.send({ type: 'jog', axis: 'z', direction: 1, velocity: 100, held: true })
    expect(sent).toHaveLength(attempts.length)
    receive({ type: 'result', id: 2, outcome: 'accepted', reason: '' })
    pass(answerTimeoutMs + 100)
    expect(panel.state.log.some((entry) => entry.text === 'No answer from the machine' && entry.context === 'Stop')).toBe(true)
  })

  it('discards incompatible snapshots and waits for fresh status after a compatible hello', () => {
    const { panel, receive, status } = rig()
    const table = { type: 'tool_table', revision: 1, tools: [] } as const
    receive(hello)
    receive({ ...table, tools: [] })
    receive(status({ link: 'live' }))
    receive({ ...hello, protocol: hello.protocol + 1, machine_units: 'inch' })
    receive({ ...table, tools: [] })
    expect(panel.state.hello).toBeNull()
    expect(panel.state.toolTable).toBeNull()
    expect(panel.state.unit).toBe('mm')
    receive(hello)
    expect(panel.state.link).toBe('connecting')
    expect(panel.state.status).toBeNull()
    receive(status({ link: 'live' }))
    expect(panel.state.link).toBe('live')
  })

  it('get increasing ids, and accepted results stay quiet', () => {
    const { panel, sent, receive } = rig()
    const first = panel.send({ type: 'home', axis: 'all' })
    const second = panel.send({ type: 'abort' })
    expect(second).toBe(first + 1)
    expect(sent).toEqual([
      { type: 'home', axis: 'all', id: first },
      { type: 'abort', id: second },
    ])
    receive({ type: 'result', id: first, outcome: 'accepted', reason: '' })
    expect(panel.state.log).toEqual([])
    expect(panel.state.notice).toBeNull()
  })

  it('every result that is not accepted is shown with its reason', () => {
    const { panel, receive } = rig()
    const jog = panel.send({ type: 'jog', axis: 'x', direction: 1, velocity: 100, held: true })
    receive({ type: 'result', id: jog, outcome: 'rejected', reason: "Can't jog: machine is off" })
    expect(panel.state.notice).toMatchObject({
      level: 'error',
      text: "Can't jog: machine is off",
      context: 'Jog X',
      count: 1,
    })
    const mdi = panel.send({ type: 'mdi', text: 'G41' })
    receive({ type: 'result', id: mdi, outcome: 'failed', reason: 'G41 is not supported by the simulator' })
    const run = panel.send({ type: 'run' })
    receive({ type: 'result', id: run, outcome: 'timed_out', reason: '' })
    expect(panel.state.log.map((entry) => [entry.context, entry.text])).toEqual([
      ['Jog X', "Can't jog: machine is off"],
      ['MDI G41', 'G41 is not supported by the simulator'],
      ['Cycle start', 'LinuxCNC did not answer'],
    ])
    panel.dismissNotice()
    expect(panel.state.notice).toBeNull()
    expect(panel.state.log).toHaveLength(3)
  })

  it('the same refusal in quick succession is counted, not repeated', () => {
    const { panel, receive, pass } = rig()
    for (let n = 0; n < 5; n += 1) {
      const id = panel.send({ type: 'jog', axis: 'x', direction: 1, velocity: 100, held: true })
      receive({ type: 'result', id, outcome: 'rejected', reason: 'X is at its positive soft limit' })
      pass(50)
    }
    expect(panel.state.log).toHaveLength(1)
    expect(panel.state.log[0]?.count).toBe(5)
    pass(10_000)
    const id = panel.send({ type: 'jog', axis: 'x', direction: 1, velocity: 100, held: true })
    receive({ type: 'result', id, outcome: 'rejected', reason: 'X is at its positive soft limit' })
    expect(panel.state.log).toHaveLength(2)
  })

  it('results for ids the panel did not send are ignored', () => {
    const { panel, receive } = rig()
    receive({ type: 'result', id: -3, outcome: 'rejected', reason: 'scene' })
    expect(panel.state.log).toEqual([])
  })

  it('tells listeners the outcome of each command', () => {
    const { panel, receive } = rig()
    const seen: string[] = []
    const remove = panel.onResult((command, result) => {
      seen.push(`${command.type} ${result.outcome}`)
    })
    receive({ type: 'result', id: panel.send({ type: 'pause' }), outcome: 'rejected', reason: 'Nothing is running' })
    remove()
    receive({ type: 'result', id: panel.send({ type: 'pause' }), outcome: 'rejected', reason: 'Nothing is running' })
    expect(seen).toEqual(['pause rejected'])
  })

  it('a command without an answer is reported once', () => {
    const { panel, pass } = rig()
    panel.send({ type: 'tool_changed' })
    pass(answerTimeoutMs - 100)
    expect(panel.state.log).toEqual([])
    pass(200)
    pass(5000)
    expect(panel.state.log.map((entry) => [entry.context, entry.text, entry.count])).toEqual([
      ['Confirm tool change', 'No answer from the machine', 1],
    ])
  })

  it('waits for a restart and its tool change as long as their approach can take', () => {
    const { panel, receive, pass, status } = rig()
    const outcomes: string[] = []
    panel.onResult((command, result) => outcomes.push(`${command.type} ${result.outcome}`))
    const start = panel.send({ type: 'start_restart', plan_id: 1 })
    pass(answerTimeoutMs + 100)
    pass(restartTimeoutMs - answerTimeoutMs - 400)
    expect(outcomes).toEqual([])
    // Accepted at the tool change: the confirmation runs the approach.
    receive({ type: 'result', id: start, outcome: 'accepted', reason: '' })
    receive(status({ mode: 'mdi' }))
    panel.send({ type: 'tool_changed' })
    pass(restartTimeoutMs - 100)
    expect(outcomes).toEqual(['start_restart accepted'])
    pass(200)
    expect(outcomes).toEqual(['start_restart accepted', 'tool_changed timed_out'])
    // The program's own tool change later answers at once.
    panel.send({ type: 'tool_changed' })
    pass(answerTimeoutMs + 1)
    expect(outcomes.at(-1)).toBe('tool_changed timed_out')
    // So does a tool change after a restart that already runs the program.
    receive({ type: 'result', id: panel.send({ type: 'start_restart', plan_id: 2 }), outcome: 'accepted', reason: '' })
    receive(status({ mode: 'auto' }))
    panel.send({ type: 'tool_changed' })
    pass(answerTimeoutMs + 1)
    expect(outcomes.slice(-2)).toEqual(['start_restart accepted', 'tool_changed timed_out'])
  })

  it('stops a height map past its deadline and keeps the partial result that answers the stop', () => {
    const { panel, sent, receive, pass } = rig()
    const results: string[] = []
    panel.onResult((command, result) => results.push(`${command.type} ${result.outcome} ${String(result.setter?.height_map?.partial)}`))
    const map = { type: 'fixed_setter', action: 'height_map', tool: 99, x: 0, y: 0, z: 0, x_min: 0, y_min: 0, x_max: 10, y_max: 0, step: 10, clearance: 0 } as const
    const id = panel.send(map)
    pass(probeAnswerMs + 1)
    // Stopped, not timed out: the map's own answer is still awaited.
    expect(sent.at(-1)).toMatchObject({ type: 'abort' })
    expect(results.filter((entry) => entry.startsWith('fixed_setter'))).toEqual([])
    const height_map = { wcs: 1, columns: 2, rows: 1, points: [{ work: [0, 0, 0.1] as const, machine: [0, 0, 1] as const }, null], partial: 'The height map was stopped.' }
    receive({ type: 'result', id, outcome: 'failed', reason: 'The height map was stopped.',
      setter: { calibration: null, measurement: null, height_map: { ...height_map, points: height_map.points.map((point) => point && { work: [...point.work], machine: [...point.machine] }) } } })
    expect(results.filter((entry) => entry.startsWith('fixed_setter'))).toEqual(['fixed_setter failed The height map was stopped.'])
    // Without any answer it times out once the stop's wait has passed too.
    const silent = panel.send(map)
    pass(probeAnswerMs + 1)
    pass(heightMapAbortMs - 10)
    expect(results.filter((entry) => entry.startsWith('fixed_setter'))).toHaveLength(1)
    pass(20)
    expect(results.at(-1)).toBe('fixed_setter timed_out undefined')
    receive({ type: 'result', id: silent, outcome: 'failed', reason: '' })
    expect(results.filter((entry) => entry.startsWith('fixed_setter'))).toHaveLength(2)
  })

  it('notifies a held control of a local timeout so it can release, and ignores a late answer', () => {
    const { panel, sent, receive, pass } = rig()
    const outcomes: string[] = []
    panel.onResult((command, result) => {
      outcomes.push(result.outcome)
      if (command.type === 'jog' && command.held && result.outcome !== 'accepted') {
        panel.send({ ...command, held: false })
      }
    })
    const id = panel.send({ type: 'jog', axis: 'x', direction: 1, velocity: 100, held: true })
    pass(answerTimeoutMs + 1)
    expect(outcomes).toEqual(['timed_out'])
    expect(sent.at(-1)).toMatchObject({ type: 'jog', held: false })
    receive({ type: 'result', id, outcome: 'accepted', reason: '' })
    expect(outcomes).toEqual(['timed_out'])
    expect(sent).toHaveLength(2)
  })

  it('names every command for the log', () => {
    expect(describeCommand({ type: 'set_machine', state: 'estop' })).toBe('E-stop')
    expect(describeCommand({ type: 'select_wcs', wcs: 7 })).toBe('Select G59.1')
    expect(describeCommand({ type: 'touch_off', axis: 'z', value: 0 })).toBe('Touch off Z')
    expect(describeCommand({ type: 'set_spindle', direction: 'stopped', rpm: 0 })).toBe('Spindle stop')
  })
})

describe('MDI history', () => {
  it('keeps message and failure keys with their fallback texts', () => {
    const { panel, receive } = rig()
    const contact = { key: 'backend.jogStop.probeContact', params: {} }
    receive({ type: 'message', level: 'info', text: 'Jog stopped: probe contact', time: 10, localization: contact })
    expect(panel.state.notice).toMatchObject({ text: 'Jog stopped: probe contact', localization: contact })
    const id = panel.send({ type: 'mdi', text: 'G0 X1' })
    const timeout = { key: 'messages.session.linuxcncNoAnswer', params: {} }
    const reason = 'LinuxCNC did not answer.'
    receive({ type: 'result', id, outcome: 'timed_out', reason, localization: timeout })
    expect(panel.state.mdi.at(-1)).toMatchObject({ reason, localization: timeout })
    expect(panel.state.log.at(-1)).toMatchObject({ text: reason, localization: timeout })
  })

  it('keeps a structured refusal and the fallback in both the MDI history and message log', () => {
    const { panel, receive } = rig()
    const id = panel.send({ type: 'mdi', text: 'G0 X1' })
    const reason = 'Home all configured axes first.'
    const refusal = { key: 'messages.policy.homeAllFirst', params: {} }
    receive({ type: 'result', id, outcome: 'rejected', reason, refusal })
    expect(panel.state.mdi.at(-1)).toMatchObject({ reason, localization: refusal })
    expect(panel.state.notice).toMatchObject({ text: reason, localization: refusal })
    expect(panel.state.log.at(-1)).toMatchObject({ text: reason, localization: refusal })
  })

  it('records each command with its result', () => {
    const { panel, receive, pass } = rig()
    const ok = panel.send({ type: 'mdi', text: 'G0 X1' })
    const bad = panel.send({ type: 'mdi', text: 'G0 Z999' })
    const lost = panel.send({ type: 'mdi', text: 'G4 P1' })
    expect(panel.state.mdi.map((entry) => entry.outcome)).toEqual(['pending', 'pending', 'pending'])
    receive({ type: 'result', id: ok, outcome: 'accepted', reason: '' })
    receive({ type: 'result', id: bad, outcome: 'failed', reason: "would exceed Z's positive limit" })
    pass(answerTimeoutMs + 300)
    expect(panel.state.mdi).toEqual([
      { id: ok, text: 'G0 X1', outcome: 'accepted', reason: '' },
      { id: bad, text: 'G0 Z999', outcome: 'failed', reason: "would exceed Z's positive limit" },
      { id: lost, text: 'G4 P1', outcome: 'timed_out', reason: 'No answer from the machine' },
    ])
  })
})

describe('link', () => {
  it('is connecting until the first status, then what the status says', () => {
    const { panel, receive, status } = rig()
    expect(panel.state.link).toBe('connecting')
    receive(status())
    expect(panel.state.link).toBe('live')
    receive(status({ link: 'stale' }))
    expect(panel.state.link).toBe('stale')
    receive(status({ link: 'down' }))
    expect(panel.state.link).toBe('down')
  })

  it('goes silent when no status arrives for a second, and live again with the next', () => {
    const { panel, receive, pass, status } = rig()
    receive(status({ seq: 1 }))
    pass(staleAfterMs - 100)
    expect(panel.state.link).toBe('live')
    pass(200)
    expect(panel.state.link).toBe('silent')
    expect(panel.state.silentFor).toBe(staleAfterMs + 100)
    // The numbers are kept, to be shown as history; they are not live.
    expect(panel.state.status?.seq).toBe(1)
    pass(3000)
    expect(panel.state.silentFor).toBe(staleAfterMs + 3100)
    receive(status({ seq: 2 }))
    expect(panel.state.link).toBe('live')
  })

  it('a status that says stale stays stale however fresh it is', () => {
    const { panel, receive, pass, status } = rig()
    for (let n = 0; n < 10; n += 1) {
      receive(status({ link: 'stale' }))
      pass(50)
      expect(panel.state.link).toBe('stale')
    }
  })
})

describe('what the machine sends', () => {
  it('clears obsolete text on live unload, replacement and reload, preserving matching snapshots', () => {
    const { panel, receive, status } = rig()
    const program = { type: 'program', file: '/parts/a.ngc', revision: 1, lines: ['G0 X1'], stamp: '0' } as const
    const loaded = status({ program: { ...status().program, file: program.file, revision: 1 } })
    const text = (): void => { receive({ ...program, lines: [...program.lines] }) }
    text(); receive(loaded)
    expect(panel.state.program?.lines).toEqual(['G0 X1'])
    receive({ ...loaded, program: { ...loaded.program, file: null } })
    expect(panel.state.program).toBeNull()
    text()
    receive({ ...loaded, program: { ...loaded.program, file: '/parts/b.ngc' } })
    expect(panel.state.program).toBeNull()
    text()
    receive({ ...loaded, program: { ...loaded.program, revision: 2 } })
    expect(panel.state.program).toBeNull()
    receive({ ...program, revision: 2, lines: ['G0 X2'], truncated: true })
    receive({ ...loaded, program: { ...loaded.program, revision: 2 } })
    expect(panel.state.program).toMatchObject({ revision: 2, lines: ['G0 X2'], truncated: true })
  })

  it('keeps historical text when stale/down statuses contain no loaded program', () => {
    const { panel, receive, status } = rig()
    receive({ type: 'program', file: '/parts/a.ngc', revision: 1, lines: ['M2'], stamp: '0' })
    for (const link of ['stale', 'down'] as const) {
      receive(status({ link, program: { ...status().program, file: null, revision: 0 } }))
      expect(panel.state.program?.file).toBe('/parts/a.ngc')
    }
  })

  it('is kept: hello, tools, offsets, program, preview, directory, messages', () => {
    const { panel, receive } = rig()
    const sim = new SimMachine({ emit: receive, clock: (): number => 42 })
    sim.handle({ type: 'ready', id: -1 })
    sim.handle({ type: 'list_directory', path: null, id: -2 })
    expect(panel.state.hello?.machine_name).toBe('Demo mill 400 × 250')
    expect(panel.state.toolTable?.tools).toHaveLength(4)
    expect(panel.state.workOffsets?.systems).toHaveLength(9)
    expect(panel.state.preview?.state).toBe('unavailable')
    expect(panel.state.directory?.entries).toHaveLength(4)
    expect(panel.state.status?.machine).toBe('estop')
    receive({ type: 'message', level: 'info', text: 'hello', time: 42 })
    expect(panel.state.notice).toMatchObject({ level: 'info', text: 'hello', time: 42, context: '' })
  })

  it('takes the display unit from the machine once', () => {
    const { panel, receive } = rig()
    const sim = new SimMachine({ emit: receive })
    sim.handle({ type: 'ready', id: -1 })
    expect(panel.state.unit).toBe('mm')
    panel.state.unit = 'inch'
    sim.handle({ type: 'ready', id: -2 })
    expect(panel.state.unit).toBe('inch')
  })

  it('reports a protocol mismatch', () => {
    const { panel, receive } = rig()
    const sim = new SimMachine({
      emit: (message): void => {
        receive(message.type === 'hello' ? { ...message, protocol: 99 } : message)
      },
    })
    sim.handle({ type: 'ready', id: -1 })
    expect(panel.state.log[0]?.text).toMatch(/^Protocol mismatch: the machine speaks version 99/)
  })
})
