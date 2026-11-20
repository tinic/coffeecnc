import { shallowRef } from 'vue'

import type { SetterState } from './protocol'

// The fixed setter's last reported state, shared by the setter controls and the tool table.
export const setterState = shallowRef<SetterState | null>(null)

// The tool was touched off against the current T98 reference.
export function touchedOff(tool: number): boolean {
  return setterState.value?.measured_tools?.includes(tool) ?? false
}
