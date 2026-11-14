// Transport between the panel and the machine.
//
// Inside the coffeecnc executable the page talks to C++ in-process: commands go out through
// WebKit's script message handler `coffeecnc`, and the executable calls
// `window.coffeecncReceive(message)` with every message for the page. There is no network socket.
// In a plain browser (UI development) the built-in simulator answers instead.

import type { FromMachine, ToMachine } from './protocol'
import { createSimulator, type SimulatorControls } from './sim'

export type Listener = (message: FromMachine) => void

export interface Transport {
  readonly simulated: boolean
  send: (message: ToMachine) => void
  listen: (listener: Listener) => void
  /** Test controls, present only on the simulator. */
  readonly simulator?: SimulatorControls
}

interface ScriptMessageHandler {
  postMessage: (body: string) => void
}

declare global {
  interface Window {
    webkit?: { messageHandlers?: { coffeecnc?: ScriptMessageHandler } }
    coffeecncReceive?: (message: FromMachine) => void
  }
}

function webkitTransport(handler: ScriptMessageHandler): Transport {
  const listeners: Listener[] = []
  window.coffeecncReceive = (message: FromMachine): void => {
    for (const listener of listeners) {
      listener(message)
    }
  }
  return {
    simulated: false,
    send: (message): void => {
      handler.postMessage(JSON.stringify(message))
    },
    listen: (listener): void => {
      listeners.push(listener)
    },
  }
}

export function createTransport(): Transport {
  const handler = window.webkit?.messageHandlers?.coffeecnc
  return handler ? webkitTransport(handler) : createSimulator(new URLSearchParams(window.location.search))
}
