import type { Status } from './protocol'
import type { LinkView, PanelState } from './store'

export function connectionBlock(link: LinkView): string | null {
  switch (link) {
    case 'live': return null
    case 'connecting': return 'No status from the machine yet'
    case 'silent': return 'No updates from the machine'
    case 'stale': return 'LinuxCNC has stopped updating its status'
    case 'down': return 'No connection to LinuxCNC'
    case 'incompatible': return 'Panel and backend protocol versions do not match'
  }
}

type MachineState = Pick<PanelState, 'link' | 'status'>

export function powerBlock({ link, status }: MachineState): string | null {
  const connection = connectionBlock(link)
  if (connection !== null || status === null) return connection ?? connectionBlock('connecting')
  if (status.machine === 'estop') return 'Machine is in E-stop'
  if (status.machine === 'off') return 'Machine is off'
  return null
}

const pausedZRecovery = 'Z is still lifted. Turn machine power off and on to clear the offset here.'

export const homingMessage = 'Homing in progress'

export function interpreterBlock(status: Status | null): string | null {
  if (status === null || status.interp === 'idle') return null
  const paused = status.interp === 'paused' || status.wait.kind === 'paused'
  if (status.mode === 'mdi') return paused ? 'MDI command paused' : 'MDI command running'
  return paused ? 'Program paused' : 'Program running'
}

/** The machine-wide reason manual controls cannot act. */
export function machineBlock(machine: MachineState, allowProgram = false): string | null {
  const power = powerBlock(machine)
  if (power !== null) return power
  if (machine.status?.paused_z?.displaced && machine.status.interp === 'idle') return pausedZRecovery
  if (machine.status?.axes.some((axis) => axis.homing)) return homingMessage
  return allowProgram && machine.status?.mode === 'auto' ? null : interpreterBlock(machine.status)
}
