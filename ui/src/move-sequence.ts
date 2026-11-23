// Runs MDI moves one after another. MDI accepts a block only while the interpreter is idle, so
// the next block goes out only when the previous one has arrived at its target. A panel Stop
// cancels the remaining blocks before the stopped position comes back from the controller.

import { reactive, shallowRef, watch } from 'vue'

import { arrived, arrivalToleranceMm, moveToleranceMm, type GoToMove } from './manual-moves'
import { t } from './i18n'
import { translateResult } from './i18n/messages'
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
  const error = shallowRef<string | (() => string)>('')
  const state = reactive<MoveSequenceState>({
    running: false, step: 0, total: 0,
    get error() { return typeof error.value === 'function' ? error.value() : error.value },
    set error(value: string) { error.value = value },
  })
  let moves: readonly GoToMove[] = []
  let command: CommandBody | null = null
  let beforeMove: Status | null = null
  let accepted = false
  let busy = false
  let idleStatuses = 0

  function end(reason: string | (() => string)): void {
    state.running = false
    error.value = reason
    command = null
  }

  function sendStep(index: number): void {
    const move = moves[index]
    if (move === undefined) { end(''); return }
    const reason = deps.block()
    if (reason !== null) { end(reason); return }
    state.step = index
    accepted = false; busy = false; idleStatuses = 0
    beforeMove = deps.status()
    const body: CommandBody = { type: 'mdi', text: move.text }
    command = body
    deps.send(body)
  }

  const stopResults = deps.onResult((sent, result) => {
    if (sent !== command || !state.running) return
    if (result.outcome !== 'accepted') { end(() => translateResult(result) || t('manual.sequence.refused')); return }
    accepted = true
    observe(deps.status())
  })

  function observe(status: Status | null): void {
    if (!state.running || status === null) return
    const reason = deps.block()
    if (reason !== null) { end(reason); return }
    if (status.link !== 'live' || status === beforeMove) return
    if (status.interp !== 'idle' || !status.motion.in_position) { busy = true; return }
    if (!accepted) return
    const move = moves[state.step]
    if (move === undefined) { end(''); return }
    // A short move can start within the feedback tolerance. An idle sample at that
    // same position cannot verify arrival, even after the command was accepted.
    const moved = busy || beforeMove !== null && !arrived(beforeMove, move.target)
    if (arrived(status, move.target, moved ? arrivalToleranceMm : moveToleranceMm)) {
      sendStep(state.step + 1)
      return
    }
    idleStatuses += 1
    if (busy) end(t('manual.sequence.stoppedEarly'))
    else if (idleStatuses > startStatuses) end(t('manual.sequence.notStarted'))
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
