import { t } from '../i18n'
import { Checks, Program, checkCommon, feedNumber, steps, type Common, type Generated } from './program'

export type SlotEnds = 'closed' | 'low' | 'high' | 'both'

export interface Slot extends Common {
  x: number
  y: number
  along: 'x' | 'y'
  ends: SlotEnds
  length: number
  width: number
  depth: number
  depthPerPass: number
  plunge: boolean
  angle: number
  entryFeed: number
}

export function slot(s: Slot): Generated {
  const checks = new Checks()
  checkCommon(s, checks)
  checks.check(Number.isFinite(s.x) && Number.isFinite(s.y), t('operations.check.centre'))
  checks.positive(s.length, 'operations.label.slotLength')
  checks.positive(s.width, 'operations.label.slotWidth')
  checks.positive(s.depth, 'operations.label.depth')
  checks.positive(s.depthPerPass, 'operations.label.depthPerPass')
  checks.positive(s.entryFeed, 'operations.label.slotEntryFeed')
  checks.check(s.width >= s.tool.diameter - 1e-9,
    t('operations.check.slotToolWidth', { diameter: s.tool.diameter.toFixed(s.unit === 'inch' ? 4 : 3) }))
  const ramp = s.ends === 'closed' && !s.plunge
  if (ramp) checks.check(s.angle > 0 && s.angle < 90, t('operations.check.slotAngle'))
  if (checks.errors.length) return { errors: checks.errors }

  // Work in whole units of the program's coordinate resolution so rounding cannot steepen
  // a ramp, advance an extra depth increment, or shorten the overtravel at an open end.
  const scale = s.unit === 'inch' ? 10_000 : 1_000
  const units = (value: number): number => Math.round(value * scale)
  const floor = (value: number): number => Math.floor(value * scale + 1e-9)
  const halfLength = units(s.length) / 2
  const halfWidth = s.width * scale / 2
  const outside = Math.ceil(s.tool.diameter * scale / 2) + 1
  const openLow = s.ends === 'low' || s.ends === 'both'
  const openHigh = s.ends === 'high' || s.ends === 'both'
  const low = openLow ? Math.floor(-halfLength - outside) : Math.ceil(-halfLength + halfWidth)
  const high = openHigh ? Math.ceil(halfLength + outside) : Math.floor(halfLength - halfWidth)
  if (s.ends === 'closed') checks.check(halfLength > halfWidth && high > low, t('operations.check.slotTravel'))
  else if (s.ends !== 'both') checks.check(2 * halfLength >= halfWidth, t('operations.check.slotOpenTravel'))
  const depth = units(s.depth)
  const stepLimit = floor(s.depthPerPass)
  const spread = Math.max(0, floor((s.width - s.tool.diameter) / 2))
  const ringStep = floor(s.tool.diameter * 0.4)
  const x = units(s.x)
  const y = units(s.y)
  const extent = Math.max(Math.abs(x), Math.abs(y)) + Math.max(Math.abs(low), Math.abs(high), spread)
  checks.check(Number.isSafeInteger(extent) && Number.isSafeInteger(depth) && Number.isSafeInteger(units(s.clearance))
    && halfLength > 0 && depth > 0 && stepLimit > 0 && units(s.clearance) > 0 && ringStep > 0
    && Number(feedNumber(s.feed, s.unit)) > 0 && Number(feedNumber(s.entryFeed, s.unit)) > 0,
  t('operations.check.slotPrecision'))
  const rampDrop = ramp ? Math.floor((high - low) * Math.tan(s.angle * Math.PI / 180) + 1e-9) : 1
  if (ramp) checks.check(rampDrop > 0, t('operations.check.slotRampRoom'))
  if (checks.errors.length) return { errors: checks.errors }

  const layers = steps(depth, stepLimit)
  const rings = spread > 0 ? steps(spread, ringStep) : 0
  const entryMoves = ramp ? steps(Math.min(depth, stepLimit), rampDrop) : 1
  if (layers * (entryMoves + 5 * rings + 7) > 12_000) return { errors: [t('operations.check.slotMoveCount')] }

  const p = new Program(s)
  const rotate = (a: number, b: number): [number, number] => s.along === 'x' ? [a, b] : [-b, a]
  const xy = (a: number, b: number): string => {
    const [dx, dy] = rotate(a, b)
    return `X${p.n((x + dx) / scale)} Y${p.n((y + dy) / scale)}`
  }
  const z = (d: number): string => `Z${p.n(-d / scale)}`
  const linear = (a: number, b: number): void => { p.cut(`G1 ${xy(a, b)}`, s.feed) }
  const arc = (a: number, b: number): void => {
    const [i, j] = rotate(0, b)
    p.cut(`G3 ${xy(a, b)} I${p.n(i / scale)} J${p.n(j / scale)}`, s.feed)
  }
  const startAt = openHigh && !openLow ? high : low
  p.start('Slot or keyway', [ // i18n-ignore
    `Centre X${p.n(s.x)} Y${p.n(s.y)}, length ${p.n(s.length)}, width ${p.n(s.width)}, along ${s.along.toUpperCase()}`, // i18n-ignore
    `Ends ${s.ends}. Depth ${p.n(s.depth)} in ${String(layers)} passes`, // i18n-ignore
    s.ends !== 'closed' ? 'Allow clear tool travel past the open stock edge' // i18n-ignore
      : ramp ? `Linear ramp, maximum ${String(s.angle)} degrees` : 'Plunge with a centre-cutting end mill', // i18n-ignore
  ])
  if (rings > 0) p.add('G91.1')
  for (let layer = 1; layer <= layers; layer += 1) {
    // Equal passes bounded by the requested maximum depth per pass.
    const before = Math.round(depth * (layer - 1) / layers)
    const after = Math.round(depth * layer / layers)
    p.add('', `(Layer ${String(layer)} of ${String(layers)})`, `G0 ${xy(startAt, 0)}`) // i18n-ignore
    let at = startAt
    if (ramp) {
      p.cut(`G1 ${z(before)}`, s.entryFeed)
      const count = steps(after - before, rampDrop)
      for (let pass = 1; pass <= count; pass += 1) {
        at = pass % 2 === 1 ? high : low
        p.cut(`G1 ${xy(at, 0)} ${z(before + Math.round((after - before) * pass / count))}`, s.entryFeed)
      }
    } else p.cut(`G1 ${z(after)}`, s.entryFeed)
    // Traverse the full centre line at depth before expanding into the sides.
    const end = at === low ? high : low
    linear(end, 0)
    // Start from the end just reached and keep both turns counterclockwise.
    const side = end === high ? 1 : -1
    for (let ring = 1; ring <= rings; ring += 1) {
      const r = side * Math.round(spread * ring / rings)
      linear(end, r)
      linear(at, r)
      arc(at, -r)
      linear(end, -r)
      arc(end, r)
    }
    p.rapidZ(s.clearance)
  }
  return { text: p.end() }
}
