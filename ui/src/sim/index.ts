// The simulator as a transport: a SimMachine driven by a timer.
//
// Development URL parameters:
//   scene=<name>   fast-forwards the machine into a state before the panel's first status
//                  (see scenes.ts for the names)
//   fault=silent|stale|down   breaks the link after the scene: no messages at all, or statuses
//                  that report `stale` or `down`

import type { Listener, Transport } from '../bridge'
import { SimAlarmLog } from './alarm-log'
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
  const alarms = new SimAlarmLog(() => localStorage)
  const machine = new SimMachine({
    emit: (message): void => {
      alarms.observe(message, Date.now())
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
  const handle = (message: Parameters<Transport['send']>[0]): void => {
    alarms.request(message)
    machine.handle(message)
    // After hello and the snapshots, as the executable sends it.
    if (message.type === 'ready' && fault !== 'silent') {
      const view = alarms.view()
      for (const listener of listeners) listener(view)
    }
  }
  return {
    simulated: true,
    send: (message): void => {
      if (!started) {
        // The scene runs once the panel listens, so that its operator messages arrive.
        started = true
        const scene = parameters.get('scene') ?? (parameters.get('tab') === 'probe' ? 'probe' : null)
        if (scene !== null) {
          applyScene(machine, scene, parameters.get('setup') ?? 'top')
        }
        handle(message)
        const wanted = parseFault(parameters.get('fault'))
        if (wanted !== 'none' || scene === 'stale') {
          setFault(wanted === 'none' ? 'silent' : wanted)
        }
        window.setInterval(() => {
          machine.tick()
        }, tickMs)
        return
      }
      handle(message)
    },
    listen: (listener): void => {
      listeners.push(listener)
    },
    simulator: { fault: (): LinkFault => fault, setFault },
  }
}
