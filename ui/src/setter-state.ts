import { shallowRef } from 'vue'

import type { SetterState } from './protocol'

// The fixed setter's last reported state, shared by the setter controls and the tool table.
export const setterState = shallowRef<SetterState | null>(null)

// The tool was touched off against the current T98 reference.
export function touchedOff(tool: number): boolean {
  return setterState.value?.measured_tools?.includes(tool) ?? false
}

// The setter's one-line status: what Touch off will do, why it can't, or what it did.
export interface SetterStatusLine { text: string; level: 'info' | 'blocked' | 'busy' | 'done' }
export const setterStatus = shallowRef<SetterStatusLine>({ text: '', level: 'info' })
