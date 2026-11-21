import { shallowRef } from 'vue'
import type { Status } from './protocol'

export interface BreakpointSelection {
  file: string
  revision: number
  line: number
  text: string
}

export const breakpointSelection = shallowRef<BreakpointSelection | null>(null)

export function selectedBreakpoint(status: Status | null): BreakpointSelection | null {
  const selected = breakpointSelection.value
  if (!selected || status?.program.file !== selected.file || status.program.revision !== selected.revision) return null
  return selected
}
