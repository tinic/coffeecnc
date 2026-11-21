import { shallowRef } from 'vue'
import type { Status } from './protocol'

export const breakpointSelection = shallowRef<{ file: string; revision: number; line: number } | null>(null)

export function selectedBreakpoint(status: Status | null): number | null {
  const selected = breakpointSelection.value
  if (!selected || status?.program.file !== selected.file || status.program.revision !== selected.revision) return null
  return selected.line
}
