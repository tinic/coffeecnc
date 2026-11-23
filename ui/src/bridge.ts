// Transport between the panel and the machine.
//
// Inside the coffeecnc executable the page talks to C++ in-process: commands go out through
// WebKit's script message handler `coffeecnc`, and the executable calls
// `window.coffeecncReceive(message)` with every message for the page. There is no network socket.
// In a plain browser (UI development) the built-in simulator answers instead. The native
// coffeecnc: URL never falls back to simulated machine state if its handler is missing.

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
  if (typeof handler?.postMessage === 'function') return webkitTransport(handler)
  if (window.location.protocol === 'coffeecnc:') {
    const listeners: Listener[] = []
    return {
      simulated: false,
      listen: (listener): void => { listeners.push(listener) },
      send: (message): void => {
        // A local refusal: no request reached the controller and no machine state is known.
        for (const listener of listeners) {
          listener({ type: 'result', id: message.id, outcome: 'rejected',
            reason: 'CoffeeCNC cannot connect to its native controller. Restart the application.' }) // i18n-ignore: protocol reason, translated where shown
        }
      },
    }
  }
  return createSimulator(new URLSearchParams(window.location.search))
}
