import { t } from './i18n'
import type { LengthUnit, Preview, ProgramCheck, Status } from './protocol'

export function currentProgramPreview(preview: Preview | null, status: Status | null): Preview | null {
  return status?.program.file && preview?.context === status.preview_context ? preview : null
}

export function describeProgramCheck(preview: Preview | null, display: LengthUnit): ProgramCheck | null {
  if (preview === null || preview.state === 'pending') return null
  const check = preview.check
  if (!check) {
    return { tools: [], frames: [], feed: null, spindle: null, units: [], travel: 'unknown',
      errors: [], warnings: [preview.error?.text ?? t('program.check.failed')] }
  }
  if (!check.units.some((unit) => unit !== display)) return check
  const warnings = [...check.warnings, display === 'mm' ? t('program.check.unitsDifferMm') : t('program.check.unitsDifferInch')]
  return { ...check, warnings }
}
