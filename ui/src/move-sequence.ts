// Runs MDI moves one after another. MDI accepts a block only while the interpreter is idle, so
// the next block goes out only when the previous one has arrived at its target. A move that ends
// anywhere else (Stop, E-stop, an error) ends the sequence: nothing further moves.

import { reactive, watch } from 'vue'

import { arrived, type GoToMove } from './manual-moves'
import type { CommandBody, Status } from './protocol'
import type { ResultListener } from './store'

/** Statuses an accepted move may stay idle and away from its target before it counts as not started. */
const startStatuses = 20

export interface MoveSequenceState {
  running: boolean
  /** Index of the move in progress. */
  step: number
  total: number
  /** Why the last sequence ended early, or ''. */
  error: string
}

export interface MoveSequence {
  state: MoveSequenceState
  start: (moves: readonly GoToMove[]) => void
  /** Sends nothing more; a move in progress continues until Stop. */
  cancel: (reason: string) => void
  dispose: () => void
}

export function createMoveSequence(deps: {
  send: (command: CommandBody) => number
  onResult: (listener: ResultListener) => () => void
  status: () => Status | null
  /** Why no further move may go out (connection, power), or null. */
  block: () => string | null
}): MoveSequence {
  const state = reactive<MoveSequenceState>({ running: false, step: 0, total: 0, error: '' })
  let moves: readonly GoToMove[] = []
  let command: CommandBody | null = null
  let phase: 'sent' | 'moving' = 'sent'
  let busy = false
  let idleStatuses = 0

  function end(reason: string): void {
    state.running = false
    state.error = reason
    command = null
  }

  function sendStep(index: number): void {
    const move = moves[index]
    if (move === undefined) { end(''); return }
    const reason = deps.block()
    if (reason !== null) { end(reason); return }
    state.step = index
    phase = 'sent'; busy = false; idleStatuses = 0
    const body: CommandBody = { type: 'mdi', text: move.text }
    command = body
    deps.send(body)
  }

  const stopResults = deps.onResult((sent, result) => {
    if (sent !== command || !state.running) return
    if (result.outcome !== 'accepted') { end(result.reason || 'The move was refused'); return }
    phase = 'moving'
    observe(deps.status())
  })

  function observe(status: Status | null): void {
    if (!state.running || phase !== 'moving' || status === null) return
    const reason = deps.block()
    if (reason !== null) { end(reason); return }
    if (status.interp !== 'idle') { busy = true; return }
    const move = moves[state.step]
    if (move === undefined || arrived(status, move.target)) { sendStep(state.step + 1); return }
    idleStatuses += 1
    if (busy) end('Go to stopped before the position was reached')
    else if (idleStatuses > startStatuses) end('The move did not start')
  }

  const stopWatch = watch(deps.status, observe, { flush: 'sync' })

  return {
    state,
    start: (list): void => {
      if (state.running) return
      moves = [...list]
      state.running = true; state.error = ''; state.total = moves.length
      sendStep(0)
    },
    cancel: (reason): void => { if (state.running) end(reason) },
    dispose: (): void => { stopResults(); stopWatch(); if (state.running) end('') },
  }
}
