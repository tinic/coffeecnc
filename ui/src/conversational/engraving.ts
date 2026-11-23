import { t } from '../i18n'
import { engravingGlyphs } from './engraving-font'
import { Checks, Program, checkCommon, feedNumber, written, type Common, type Generated } from './program'

export interface Engraving extends Common {
  label: string
  /** Left end of the lettering baseline in work coordinates. */
  x: number
  y: number
  height: number
  spacing: number
  angle: number
  depth: number
  entryFeed: number
}

const maxLength = 48
const uppercase = (label: string): string => label.trim().replace(/[a-z]/g, (character) => character.toUpperCase())

export function engraving(c: Engraving): Generated {
  const checks = new Checks()
  checkCommon(c, checks)
  checks.check(Number.isFinite(c.x) && Number.isFinite(c.y), t('operations.check.position'))
  checks.check(Number.isFinite(c.angle), t('operations.check.engravingRotation'))
  checks.positive(c.height, 'operations.label.engravingHeight')
  checks.positive(c.depth, 'operations.label.depth')
  checks.positive(c.entryFeed, 'operations.label.entryFeed')
  checks.check(Number.isFinite(c.spacing) && c.spacing >= 0, t('operations.check.engravingSpacing'))
  const label = uppercase(c.label)
  checks.check(label.length > 0 && label.length <= maxLength, t('operations.check.engravingLength', { max: String(maxLength) }))
  const glyphs = Array.from(label, (letter) => engravingGlyphs[letter])
  checks.check(glyphs.every((shape) => shape !== undefined), t('operations.check.engravingCharacters'))
  const extent = Math.max(Math.abs(c.x), Math.abs(c.y)) + c.height + label.length * (c.height + c.spacing)
  const largest = Math.max(extent, c.depth, c.clearance, c.rpm, c.feed, c.entryFeed)
  checks.check(Number.isSafeInteger(Math.ceil(largest * 10_000)) && written(c.height / 10, c.unit) > 0 && written(c.depth, c.unit) > 0 && written(c.clearance, c.unit) > 0
    && Number(feedNumber(c.entryFeed, c.unit)) > 0 && Number(feedNumber(c.feed, c.unit)) > 0 && Math.round(c.rpm) > 0,
  t('operations.check.engravingPrecision'))
  if (checks.errors.length) return { errors: checks.errors }

  const p = new Program(c)
  const scale = c.height / 10
  const angle = (c.angle % 360) * Math.PI / 180
  const cosine = Math.cos(angle)
  const sine = Math.sin(angle)
  const xy = (u: number, v: number): string =>
    `X${p.n(c.x + u * cosine - v * sine)} Y${p.n(c.y + u * sine + v * cosine)}`
  p.start('Engraving', [ // i18n-ignore
    `Lettering: ${label}`, // i18n-ignore
    `Baseline ${xy(0, 0)}, height ${p.n(c.height)}, depth ${p.n(c.depth)}, rotation ${String(c.angle % 360)} degrees`, // i18n-ignore
  ])

  let advance = 0
  for (const shape of glyphs) {
    if (shape === undefined) return { errors: [t('operations.check.engravingCharacters')] }
    for (const stroke of shape.strokes) {
      const [first, ...rest] = stroke
      if (first === undefined) continue
      let last = xy(advance + first[0] * scale, first[1] * scale)
      p.add(`G0 ${last}`)
      p.cut(`G1 Z${p.n(-c.depth)}`, c.entryFeed)
      for (const [x, y] of rest) {
        const next = xy(advance + x * scale, y * scale)
        if (next === last) continue
        p.cut(`G1 ${next}`, c.feed)
        last = next
      }
      p.rapidZ(c.clearance)
    }
    advance += shape.width * scale + c.spacing
  }
  return { text: p.end() }
}
