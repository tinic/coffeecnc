import { t } from './i18n'
import { translateMessage } from './i18n/messages'
import type { LengthUnit, Preview, ProgramCheck, Status } from './protocol'

export function currentProgramPreview(preview: Preview | null, status: Status | null): Preview | null {
  return status?.program.file && preview?.context === status.preview_context ? preview : null
}

/**
 * The lines a breakpoint (Run to line) can be set on: the motion lines of a checked preview.
 * A preview that ended early ('failed') still offers its path up to that end, because the run
 * reaches those lines before anything the preview could not follow; Run then asks for the
 * review of the partial check. Restart from line needs a complete preview and is not decided here.
 */
export function breakpointLines(preview: Preview | null): Set<number> {
  if (!preview?.check || (preview.state !== 'ready' && preview.state !== 'failed')) return new Set()
  return new Set(preview.segments.map((segment) => segment.line))
}

export function describeProgramCheck(preview: Preview | null, display: LengthUnit): ProgramCheck | null {
  if (preview === null || preview.state === 'pending') return null
  const check = preview.check
  if (!check) {
    return { tools: [], frames: [], feed: null, spindle: null, units: [], travel: 'unknown',
      errors: [], warnings: [preview.error ? translateMessage(preview.error.text, preview.error.localization)
        : t('program.check.failed')] }
  }
  if (!check.units.some((unit) => unit !== display)) return check
  const warnings = [...check.warnings, display === 'mm' ? t('program.check.unitsDifferMm') : t('program.check.unitsDifferInch')]
  return { ...check, warnings }
}
