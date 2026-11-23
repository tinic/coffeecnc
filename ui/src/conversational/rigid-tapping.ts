import { t } from '../i18n'
import { checkHoleLayout, holes, type HoleLayout } from './drilling'
import { Checks, Program, checkCommon, unitName, written, type Common, type Generated } from './program'

export interface RigidTapping extends Common, HoleLayout {
  /** Nominal reversal point below the stock top, excluding reversal travel. */
  depth: number
  pitch: number
  pilotDepth: number
  /** Height above Z0 where synchronised travel starts and returns. */
  retract: number
}

export function rigidTapping(c: RigidTapping): Generated {
  const limits = c.rigidTap
  if (limits === undefined || !Number.isFinite(limits.max_rpm) || limits.max_rpm < 1 ||
    !Number.isFinite(limits.reversal_turns) || limits.reversal_turns <= 0 ||
    !Number.isFinite(limits.max_z_feed_mm_min) || limits.max_z_feed_mm_min <= 0)
    return { errors: [t('operations.check.tapUnavailable')] }

  const pitch = Number(c.pitch.toFixed(6))
  const rpm = Math.round(c.rpm)
  const feed = pitch * rpm
  const checks = new Checks()
  const program = { ...c, feed }
  checkCommon(program, checks)
  checkHoleLayout(c, checks)
  const depth = written(c.depth, c.unit)
  const start = written(c.retract, c.unit)
  const clearance = written(c.clearance, c.unit)
  checks.positive(pitch, 'operations.label.tapPitch')
  checks.positive(depth, 'operations.label.holeDepth')
  checks.positive(start, 'operations.label.tapStartHeight')
  checks.positive(c.pilotDepth, 'operations.label.tapPilotDepth')
  checks.check(rpm >= 1 && rpm <= limits.max_rpm,
    t('operations.check.tapRpm', { rpm: String(limits.max_rpm) }))
  const unitScale = c.unit === 'inch' ? 25.4 : 1
  const maxFeed = limits.max_z_feed_mm_min / unitScale
  checks.check(feed <= maxFeed, t('operations.check.tapZFeed', {
    feed: `${String(Number(maxFeed.toFixed(2)))} ${unitName(c.unit)}/min`,
  }))
  const reserve = pitch * limits.reversal_turns
  const grid = c.unit === 'mm' ? 1000 : 10000
  const lowest = Math.ceil((depth + reserve) * grid) / grid
  const highest = Math.ceil((start + reserve) * grid) / grid
  checks.check(c.pilotDepth > lowest,
    t('operations.check.tapPilotDepth', { depth: `${String(lowest)} ${unitName(c.unit)}` }))
  checks.check(Number.isFinite(highest) && clearance >= highest,
    t('operations.check.tapClearance', { height: `${String(highest)} ${unitName(c.unit)}` }))
  if (checks.errors.length) return { errors: checks.errors }

  const p = new Program(program)
  p.start('Rigid tapping', [ // i18n-ignore
    `Right-hand tap, G33.1, pitch ${String(pitch)}, nominal reversal at Z-${p.n(depth)}`, // i18n-ignore
    `Qualified reversal clearance: through Z-${p.n(lowest)} below and Z${p.n(highest)} above`, // i18n-ignore
  ], true)
  p.add('M51 P0')
  for (const [x, y] of holes(c)) {
    p.rapidXY(x, y)
    p.rapidZ(start)
    p.add(`G33.1 Z${p.n(-depth)} K${String(pitch)}`)
    p.rapidZ(clearance)
  }
  return { text: p.end(false, ['M51 P1']) }
}
