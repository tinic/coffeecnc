import type { LengthUnit } from './protocol'
import { validMachineId } from './tool-model-settings'

export interface OperatorPreferences {
  version: 1
  startup_units: 'machine' | LengthUnit
  enabled_wcs: number[]
  /** RPM preset only; zero leaves the speed unset. Never starts the spindle. */
  default_spindle_rpm: number
  /** mm/min, independent of display units. */
  default_jog_velocity: number
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
    default_spindle_rpm: 0, default_jog_velocity: 600 }
}

export function decodeOperatorPreferences(value: unknown): OperatorPreferences | null {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) return null
  const v = value as Record<string, unknown>
  const keys = ['version', 'startup_units', 'enabled_wcs', 'default_spindle_rpm', 'default_jog_velocity']
  if (Object.keys(v).length !== keys.length || Object.keys(v).some((key) => !keys.includes(key)) ||
      v['version'] !== 1 || !['machine', 'mm', 'inch'].includes(String(v['startup_units']))) return null
  const systems = v['enabled_wcs']
  if (!Array.isArray(systems) || systems.length < 1 || systems.length > 9 ||
      systems.some((wcs: unknown) => typeof wcs !== 'number' || !Number.isInteger(wcs) || wcs < 1 || wcs > 9) ||
      new Set(systems).size !== systems.length) return null
  const rpm = v['default_spindle_rpm'], jog = v['default_jog_velocity']
  if (typeof rpm !== 'number' || !Number.isFinite(rpm) || rpm < 0 || rpm > 1e9 ||
      typeof jog !== 'number' || !Number.isFinite(jog) || jog <= 0 || jog > 1e9) return null
  return { version: 1, startup_units: v['startup_units'] as OperatorPreferences['startup_units'],
    enabled_wcs: [...(systems as number[])], default_spindle_rpm: rpm, default_jog_velocity: jog }
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
