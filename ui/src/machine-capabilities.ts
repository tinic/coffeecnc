import { fromDisplay } from './units'
import type { AxisLetter, Hello, LengthUnit } from './protocol'

/** Older v1 peers retain the original homing requirement. Only explicit false relaxes it. */
export function requiresHoming(hello: Hello | null): boolean { return hello?.homing_required !== false }

/** A missing axis in an advertised map is unavailable, not permission to use a larger limit. */
export function axisJogVelocity(hello: Hello | null, axis: AxisLetter, requested: number): number {
  if (hello === null || !hello.axes.includes(axis) || !Number.isFinite(requested) || requested <= 0) return 0
  const axisLimit = hello.axis_max_jog_velocity === undefined ? hello.max_jog_velocity : hello.axis_max_jog_velocity[axis]
  if (axisLimit === undefined || !Number.isFinite(axisLimit) || axisLimit <= 0 ||
    !Number.isFinite(hello.max_jog_velocity) || hello.max_jog_velocity <= 0) return 0
  return Math.min(requested, hello.max_jog_velocity, axisLimit)
}

/** Keep configured native presets; the other unit gets round values, never rounded labels
 * for different physical distances. Do not offer a larger step than the configured maximum. */
export function displayJogIncrements(hello: Hello | null, unit: LengthUnit): number[] {
  if (hello === null) return []
  const configured = hello.jog_increments.filter((value) => Number.isFinite(value) && value > 0)
  if (unit === hello.machine_units) return configured
  const largest = Math.max(0, ...configured)
  const values = unit === 'inch' ? [0.1, 0.05, 0.01, 0.005, 0.001, 0.0005, 0.0001] : [10, 5, 1, 0.5, 0.1, 0.05, 0.01, 0.005]
  return values.map((value) => fromDisplay(value, unit)).filter((value) => value <= largest)
}
