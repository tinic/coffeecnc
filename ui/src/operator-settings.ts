import type { LengthUnit, Vec3 } from './protocol'
import { decodeLanguage, type Language } from './i18n'
import { validMachineId } from './tool-model-settings'

export type ProgramLayout = 'path' | 'text' | 'split'
export const panelThemes = ['light', 'dark', 'sandstone', 'harbor'] as const
export type PanelTheme = typeof panelThemes[number]

/** Message keys of the theme names; `t(themeLabels[theme])` where shown. */
export const themeLabels = {
  light: 'status.theme.light', dark: 'status.theme.dark', sandstone: 'status.theme.sandstone', harbor: 'status.theme.harbor',
} as const satisfies Record<PanelTheme, string>

export function isDarkTheme(theme: PanelTheme): boolean {
  return theme === 'dark' || theme === 'harbor'
}

export function alternateTheme(theme: PanelTheme): PanelTheme {
  switch (theme) {
    case 'light': return 'dark'
    case 'dark': return 'light'
    case 'sandstone': return 'harbor'
    case 'harbor': return 'sandstone'
  }
}

export function decodeProgramLayout(value: unknown): ProgramLayout | null {
  return value === 'path' || value === 'text' || value === 'split' ? value : null
}

export function decodeTheme(value: unknown): PanelTheme | null {
  return panelThemes.find((theme) => theme === value) ?? null
}

export interface OperatorPreferences {
  version: 1
  startup_units: 'machine' | LengthUnit
  enabled_wcs: number[]
  /** RPM preset only; zero leaves the speed unset. Never starts the spindle. */
  default_spindle_rpm: number
  /** mm/min, independent of display units. */
  default_jog_velocity: number
  /** mm above work Z zero at contact; offered by the tool length touch-off dialog. */
  tool_reference_height: number
  program_layout: ProgramLayout
  theme: PanelTheme
  /** Panel text language; numbers, G-code and units keep their technical form. */
  language: Language
  /** Programs opened most recently first, absolute paths on the machine; at most eight. */
  recent_programs: string[]
  /** Positions the operator saved in Manual, machine coordinates in mm; at most sixteen. */
  saved_positions: SavedPosition[]
  /** Machine Z in mm that Go to lifts to before X and Y move; null: Z max less 0.5 mm. */
  go_to_safe_z: number | null
}

export interface SavedPosition {
  /** At most 64 UTF-8 bytes, not blank, no control characters. */
  name: string
  position: Vec3
}

export const maxSavedPositions = 16
export const maxPositionNameBytes = 64

export const maxRecentPrograms = 8

/** `path` first, then the others without it, at most eight. */
export function withRecentProgram(recent: readonly string[], path: string): string[] {
  return [path, ...recent.filter((entry) => entry !== path)].slice(0, maxRecentPrograms)
}

function decodeRecentPrograms(value: unknown): string[] {
  return Array.isArray(value) && value.length <= maxRecentPrograms &&
    value.every((path: unknown) => typeof path === 'string' && path.startsWith('/') && path.length <= 4096 && !path.includes('\0'))
    ? [...(value as string[])] : []
}

export function validPositionName(name: unknown): name is string {
  // eslint-disable-next-line no-control-regex
  return typeof name === 'string' && /[^ ]/.test(name) && !/[\u0000-\u001f\u007f]/.test(name) &&
    new TextEncoder().encode(name).length <= maxPositionNameBytes
}

function decodeSavedPositions(value: unknown): SavedPosition[] {
  if (!Array.isArray(value) || value.length > maxSavedPositions) return []
  const positions: SavedPosition[] = []
  for (const entry of value as unknown[]) {
    if (typeof entry !== 'object' || entry === null || Array.isArray(entry)) return []
    const e = entry as Record<string, unknown>
    const position = e['position']
    if (Object.keys(e).some((key) => key !== 'name' && key !== 'position') || !validPositionName(e['name']) ||
        !Array.isArray(position) || position.length !== 3 ||
        position.some((axis: unknown) => typeof axis !== 'number' || !Number.isFinite(axis) || Math.abs(axis) > 1e6)) return []
    positions.push({ name: e['name'], position: [...(position as Vec3)] })
  }
  return positions
}

export type OperatorSettings = {
  type: 'operator_settings'
  machine_id: string | null
  revision: number
  reason: string
} & ({ availability: 'available'; machine_id: string; document: OperatorPreferences }
  | { availability: 'session_only' | 'unavailable'; document: null })

export function defaultOperatorPreferences(): OperatorPreferences {
  return { version: 1, startup_units: 'machine', enabled_wcs: [1, 2, 3, 4, 5, 6, 7, 8, 9],
    default_spindle_rpm: 0, default_jog_velocity: 600, tool_reference_height: 0, program_layout: 'path', theme: 'light', language: 'en',
    recent_programs: [], saved_positions: [], go_to_safe_z: null }
}

export function decodeOperatorPreferences(value: unknown): OperatorPreferences | null {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) return null
  const v = value as Record<string, unknown>
  const keys = ['version', 'startup_units', 'enabled_wcs', 'default_spindle_rpm', 'default_jog_velocity']
  if (keys.some((key) => !(key in v)) || Object.keys(v).some((key) => ![...keys, 'tool_reference_height', 'program_layout', 'theme', 'language', 'recent_programs', 'saved_positions', 'go_to_safe_z'].includes(key)) ||
      v['version'] !== 1 || !['machine', 'mm', 'inch'].includes(String(v['startup_units']))) return null
  const systems = v['enabled_wcs']
  if (!Array.isArray(systems) || systems.length < 1 || systems.length > 9 ||
      systems.some((wcs: unknown) => typeof wcs !== 'number' || !Number.isInteger(wcs) || wcs < 1 || wcs > 9) ||
      new Set(systems).size !== systems.length) return null
  const rpm = v['default_spindle_rpm'], jog = v['default_jog_velocity']
  // Older v1 documents have no reference height. Preserve every saved preference.
  const reference = 'tool_reference_height' in v ? v['tool_reference_height'] : 0
  if (typeof rpm !== 'number' || !Number.isFinite(rpm) || rpm < 0 || rpm > 1e9 ||
      typeof jog !== 'number' || !Number.isFinite(jog) || jog <= 0 || jog > 1e9 ||
      typeof reference !== 'number' || !Number.isFinite(reference) || reference < 0 || reference > 1e9) return null
  return { version: 1, startup_units: v['startup_units'] as OperatorPreferences['startup_units'],
    enabled_wcs: [...(systems as number[])], default_spindle_rpm: rpm, default_jog_velocity: jog,
    tool_reference_height: reference, program_layout: decodeProgramLayout(v['program_layout']) ?? 'path',
    theme: decodeTheme(v['theme']) ?? 'light', language: decodeLanguage(v['language']) ?? 'en', recent_programs: decodeRecentPrograms(v['recent_programs']),
    saved_positions: decodeSavedPositions(v['saved_positions']), go_to_safe_z: decodeSafeZ(v['go_to_safe_z']) }
}

/** A damaged or missing safe Z falls back to the default rule without losing the rest. */
function decodeSafeZ(value: unknown): number | null {
  return typeof value === 'number' && Number.isFinite(value) && Math.abs(value) <= 1e9 ? value : null
}

export function decodeOperatorSettings(value: unknown): OperatorSettings | null {
  if (typeof value !== 'object' || value === null) return null
  const v = value as Record<string, unknown>
  if (v['type'] !== 'operator_settings' || typeof v['reason'] !== 'string' ||
      typeof v['revision'] !== 'number' || !Number.isSafeInteger(v['revision']) || v['revision'] < 0) return null
  if (v['availability'] === 'available') {
    const document = decodeOperatorPreferences(v['document'])
    if (!validMachineId(v['machine_id']) || !document) return null
    return { type: 'operator_settings', availability: 'available', machine_id: v['machine_id'],
      revision: v['revision'], reason: v['reason'], document }
  }
  if (!['session_only', 'unavailable'].includes(String(v['availability'])) || v['revision'] !== 0 ||
      v['document'] !== null || !(v['machine_id'] === null || validMachineId(v['machine_id']))) return null
  return { type: 'operator_settings', availability: v['availability'] as 'session_only' | 'unavailable',
    machine_id: v['machine_id'], revision: 0, reason: v['reason'], document: null }
}

/** Active system stays visible even if a program selects one omitted from shortcuts. */
export function visibleWorkSystems(enabled: readonly number[], active: number | undefined, capacity = 5): { primary: number[]; more: number[] } {
  const all = [...new Set([...enabled, ...(active !== undefined && active >= 1 && active <= 9 ? [active] : [])])].sort((a, b) => a - b)
  const primary = all.slice(0, capacity)
  if (active !== undefined && all.includes(active) && !primary.includes(active)) {
    primary[primary.length - 1] = active
    primary.sort((a, b) => a - b)
  }
  return { primary, more: all.filter((wcs) => !primary.includes(wcs)) }
}
