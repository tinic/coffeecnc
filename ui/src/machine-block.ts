import { t } from './i18n'
import type { Status } from './protocol'
import type { LinkView, PanelState } from './store'

export function connectionBlock(link: LinkView): string | null {
  switch (link) {
    case 'live': return null
    case 'connecting': return t('status.machineBlock.connecting')
    case 'silent': return t('status.machineBlock.silent')
    case 'stale': return t('status.machineBlock.stale')
    case 'down': return t('status.machineBlock.down')
    case 'incompatible': return t('status.machineBlock.incompatible')
  }
}

type MachineState = Pick<PanelState, 'link' | 'status'>

export function powerBlock({ link, status }: MachineState): string | null {
  const connection = connectionBlock(link)
  if (connection !== null || status === null) return connection ?? connectionBlock('connecting')
  if (status.machine === 'estop') return t('status.machineBlock.estop')
  if (status.machine === 'off') return t('status.machineBlock.off')
  return null
}

/** In the panel's language; call it where the text is shown. */
export function homingMessage(): string { return t('status.machineBlock.homing') }

export function interpreterBlock(status: Status | null): string | null {
  if (status === null || status.interp === 'idle') return null
  const paused = status.interp === 'paused' || status.wait.kind === 'paused'
  if (status.mode === 'mdi') return t(paused ? 'status.machineBlock.mdiPaused' : 'status.machineBlock.mdiRunning')
  return t(paused ? 'status.machineBlock.programPaused' : 'status.machineBlock.programRunning')
}

/** The machine-wide reason manual controls cannot act. */
export function machineBlock(machine: MachineState, allowProgram = false): string | null {
  const power = powerBlock(machine)
  if (power !== null) return power
  if (machine.status?.paused_z?.displaced && machine.status.interp === 'idle') return t('status.machineBlock.pausedZRecovery')
  if (machine.status?.axes.some((axis) => axis.homing)) return homingMessage()
  return allowProgram && machine.status?.mode === 'auto' ? null : interpreterBlock(machine.status)
}
