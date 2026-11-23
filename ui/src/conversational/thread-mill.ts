import { t } from '../i18n'
import { Checks, Program, checkCommon, feedNumber, unitName, written, type Common, type Generated } from './program'

export interface ThreadMill extends Common {
  x: number
  y: number
  majorDiameter: number
  pilotDiameter: number
  /** Usable straight depth of the predrilled hole, excluding the drill point. */
  pilotDepth: number
  depth: number
  pitch: number
  leftHand: boolean
  passes: number
  entryFeed: number
}

// Keep fractional pitches (such as 1/28 in) and the end point of a partial turn.
const word = (value: number): string => String(Number(value.toFixed(6)))
const rounded = (value: number): number => Number(word(value))
const maxTurns = 500

export function threadMill(c: ThreadMill): Generated {
  const checks = new Checks()
  checkCommon(c, checks)
  checks.check(Number.isFinite(c.x) && Number.isFinite(c.y), t('operations.check.centre'))
  checks.positive(c.majorDiameter, 'operations.label.threadMajor')
  checks.positive(c.pilotDiameter, 'operations.label.threadPilot')
  checks.positive(c.pilotDepth, 'operations.label.threadPilotDepth')
  checks.positive(c.depth, 'operations.label.threadDepth')
  checks.positive(c.pitch, 'operations.label.threadPitch')
  checks.positive(c.entryFeed, 'operations.label.entryFeed')
  checks.count(c.passes, 'operations.label.threadPasses', 8)
  if (checks.errors.length) return { errors: checks.errors }

  const x = rounded(c.x)
  const y = rounded(c.y)
  const depth = rounded(c.depth)
  const pitch = c.pitch
  const leadTravel = rounded(pitch / 4)
  const lowest = rounded(depth + leadTravel)
  const turns = depth / pitch
  const fullTurns = Math.floor(turns + 1e-10)
  const remaining = Math.max(0, turns - fullTurns)
  const pilot = (c.pilotDiameter - c.tool.diameter) / 2
  const engagement = (c.majorDiameter - c.pilotDiameter) / 2
  checks.check(written(pilot, c.unit) > 0 && written(engagement / c.passes, c.unit) > 0,
    t('operations.check.threadDiameters'))
  checks.check(rounded(c.pilotDepth) > lowest, t('operations.check.threadPilotDepth', { depth: `${word(lowest)} ${unitName(c.unit)}` }))
  checks.check(written(c.clearance, c.unit) > leadTravel && leadTravel > 0 &&
    Number(feedNumber(c.feed, c.unit)) > 0 && Number(feedNumber(c.entryFeed, c.unit)) > 0 && Math.round(c.rpm) > 0,
    t('operations.check.threadPrecision'))
  checks.check(turns >= 1 && turns <= maxTurns, t('operations.check.threadTurns', { max: String(maxTurns) }))
  if (checks.errors.length) return { errors: checks.errors }

  const p = new Program(c)
  const direction = c.leftHand ? -1 : 1
  const startZ = c.leftHand ? 0 : -depth
  const endZ = c.leftHand ? -depth : 0
  p.start('Internal thread milling', [ // i18n-ignore
    `Single-form cutter, ${c.leftHand ? 'left-hand' : 'right-hand'}, single-start thread`, // i18n-ignore
    `Major diameter ${word(c.majorDiameter)}, pilot diameter ${word(c.pilotDiameter)}, pitch ${word(pitch)}`, // i18n-ignore
    `Thread depth ${word(depth)}, lowest tool position Z-${word(lowest)}`, // i18n-ignore
  ])
  // Exact path keeps the lead-in clear and the programmed thread radius through each turn.
  p.add('G61 G91.1', `G0 X${word(x)} Y${word(y)}`)

  const xy = (px: number, py: number): string => `X${word(px)} Y${word(py)}`
  const arc = (px: number, py: number, z: number, i: number, j: number): void => {
    p.cut(`G3 ${xy(px, py)} Z${word(z)} I${word(i)} J${word(j)}`, c.feed)
  }

  const phase = 2 * Math.PI * remaining
  for (let pass = 1; pass <= c.passes; pass += 1) {
    const wallX = rounded(x + pilot + engagement * pass / c.passes)
    const radius = wallX - x
    const finalX = rounded(x + radius * Math.cos(phase))
    const finalY = rounded(y + radius * Math.sin(phase))
    if (remaining > 1e-10 && finalX === wallX && finalY === y)
      return { errors: [t('operations.check.threadPrecision')] }
    p.add('', `(Radial pass ${String(pass)} of ${String(c.passes)})`) // i18n-ignore
    p.cut(`G1 Z${word(startZ - direction * leadTravel)}`, c.entryFeed)
    arc(wallX, y, startZ, radius / 2, 0)
    for (let turn = 1; turn <= fullTurns; turn += 1)
      arc(wallX, y, startZ + direction * pitch * turn, -radius, 0)
    if (remaining > 1e-10) arc(finalX, finalY, endZ, -radius, 0)
    arc(x, y, endZ + direction * leadTravel, (x - finalX) / 2, (y - finalY) / 2)
    p.rapidZ(c.clearance)
  }
  return { text: p.end() }
}
