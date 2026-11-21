// The simulator as a transport: a SimMachine driven by a timer.
//
// Development URL parameters:
//   scene=<name>   fast-forwards the machine into a state before the panel's first status
//                  (see scenes.ts for the names)
//   fault=silent|stale|down   breaks the link after the scene: no messages at all, or statuses
//                  that report `stale` or `down`

import type { Listener, Transport } from '../bridge'
import { SimMachine, tickMs, type LinkFault } from './machine'
import { applyScene } from './scenes'

export interface SimulatorControls {
  fault: () => LinkFault
  setFault: (fault: LinkFault) => void
}

const faults: readonly LinkFault[] = ['none', 'silent', 'stale', 'down']

function parseFault(text: string | null): LinkFault {
  return faults.find((fault) => fault === text) ?? 'none'
}

export function createSimulator(parameters: URLSearchParams): Transport {
  const listeners: Listener[] = []
  const machine = new SimMachine({
    emit: (message): void => {
      for (const listener of listeners) {
        listener(message)
      }
    },
  })
  let fault: LinkFault = 'none'
  let started = false
  const setFault = (next: LinkFault): void => {
    fault = next
    machine.setFault(next)
  }
  return {
    simulated: true,
    send: (message): void => {
      if (!started) {
        // The scene runs once the panel listens, so that its operator messages arrive.
        started = true
        const scene = parameters.get('scene')
        if (scene !== null) {
          applyScene(machine, scene, parameters.get('setup') ?? 'top')
        }
        machine.handle(message)
        const wanted = parseFault(parameters.get('fault'))
        if (wanted !== 'none' || scene === 'stale') {
          setFault(wanted === 'none' ? 'silent' : wanted)
        }
        window.setInterval(() => {
          machine.tick()
        }, tickMs)
        return
      }
      machine.handle(message)
    },
    listen: (listener): void => {
      listeners.push(listener)
    },
    simulator: { fault: (): LinkFault => fault, setFault },
  }
}
