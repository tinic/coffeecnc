import type { AxisLetter, Hello } from './protocol'

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
