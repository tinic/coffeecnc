import type {
  CommandBody,
  CommandResult,
  FromMachine,
  OperatorMessage,
  Preview,
  Status,
} from '../src/protocol'
import { defaultFiles, programDirectory } from '../src/sim/files'
import { SimMachine } from '../src/sim/machine'

export interface Rig {
  sim: SimMachine
  out: FromMachine[]
  /** Sends a command and returns its result. */
  send: (command: CommandBody) => CommandResult
  status: () => Status
  messages: () => OperatorMessage[]
  errors: () => string[]
  preview: () => Preview
  /** Machine on and all axes homed. */
  home: () => void
  /** Loads `text` as a program and waits for its preview. */
  load: (text: string) => void
  /** Runs until the interpreter is idle, confirming tool changes and resuming pauses. */
  finish: () => void
}

export const testProgram = `${programDirectory}/test.ngc`

export function rig(): Rig {
  const out: FromMachine[] = []
  const files = defaultFiles()
  const sim = new SimMachine({
    emit: (message): void => {
      out.push(message)
    },
    clock: (): number => 0,
    files,
  })
  let id = 0
  const send = (command: CommandBody): CommandResult => {
    id += 1
    sim.handle({ ...command, id })
    const result = out.findLast((message) => message.type === 'result' && message.id === id)
    if (result?.type !== 'result') {
      throw new Error(`no result for ${command.type}`)
    }
    return result
  }
  const messages = (): OperatorMessage[] => out.filter((message) => message.type === 'message')
  return {
    sim,
    out,
    send,
    status: (): Status => sim.status(),
    messages,
    errors: (): string[] =>
      messages()
        .filter((message) => message.level === 'error')
        .map((message) => message.text),
    preview: (): Preview => {
      const preview = out.findLast((message) => message.type === 'preview')
      if (preview?.type !== 'preview') {
        throw new Error('no preview')
      }
      return preview
    },
    home: (): void => {
      send({ type: 'set_machine', state: 'off' })
      send({ type: 'set_machine', state: 'on' })
      send({ type: 'home', axis: 'all' })
      sim.advanceUntil((status) => status.axes.every((axis) => axis.homed), 60_000)
    },
    load: (text: string): void => {
      files.set(testProgram, text)
      send({ type: 'open_program', path: testProgram })
      sim.advance(400)
    },
    finish: (): void => {
      for (let n = 0; n < 200_000; n += 1) {
        const status = sim.status()
        if (status.interp === 'idle') {
          return
        }
        if (status.wait.kind === 'tool_change') {
          send({ type: 'tool_changed' })
        } else if (status.wait.kind === 'paused') {
          send({ type: 'resume' })
        }
        sim.tick()
      }
      throw new Error('program did not finish')
    },
  }
}

/** The work position, rounded to micrometres. */
export function work(status: Status): number[] {
  return status.axes.map((axis) => Math.round(axis.work * 1000) / 1000 + 0)
}

export function machinePosition(status: Status): number[] {
  return status.axes.map((axis) => Math.round(axis.machine * 1000) / 1000 + 0)
}
