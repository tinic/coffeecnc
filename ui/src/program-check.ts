import type { LengthUnit, Preview, ProgramCheck, Status } from './protocol'
import { formatLength } from './units'

export function airLiftLabel(lift: number, unit: LengthUnit): string {
  return `+${formatLength(lift, unit)} ${unit === 'mm' ? 'mm' : 'in'}`
}

export function currentProgramPreview(preview: Preview | null, status: Status | null): Preview | null {
  return status?.program.file && preview?.context === status.preview_context ? preview : null
}

export function describeProgramCheck(preview: Preview | null, display: LengthUnit): ProgramCheck | null {
  if (preview === null || preview.state === 'pending') return null
  const check = preview.check
  if (!check) {
    return { tools: [], frames: [], feed: null, spindle: null, units: [], travel: 'unknown',
      errors: [], warnings: [preview.error?.text ?? 'CoffeeCNC could not check this program.'] }
  }
  if (!check.units.some((unit) => unit !== display)) return check
  const warnings = [...check.warnings, `Program units differ from the ${display === 'mm' ? 'millimetre' : 'inch'} display.`]
  return { ...check, warnings }
}
