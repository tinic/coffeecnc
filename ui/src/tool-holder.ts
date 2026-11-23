import { t } from './i18n'

// Dimensions are nominal envelopes in millimetres. TTS uses a 3/4 in. shank.
// ER nut diameters follow the standard REGO-FIX envelopes (28, 34 and 50 mm).
const bores = [4, 5, 6, 7, 8, 9, 10, 11, 12] as const
export const holderChoices = [
  { id: 'r8' }, { id: 'er16' }, { id: 'er20' }, { id: 'er32' }, { id: 'bt30' }, { id: 'generic' },
  ...bores.map((bore) => ({ id: `tts-${String(bore)}` as `tts-${typeof bores[number]}` })), // i18n-ignore
  { id: 'tts-er16' }, { id: 'tts-er20' }, { id: 'tts-er32' },
] as const
export type ToolHolder = typeof holderChoices[number]['id']

export function isToolHolder(value: unknown): value is ToolHolder {
  return holderChoices.some(({ id }) => id === value)
}

const holderNames = {
  r8: 'tools.holder.r8', er16: 'tools.holder.er16', er20: 'tools.holder.er20', er32: 'tools.holder.er32',
  bt30: 'tools.holder.bt30', generic: 'tools.holder.generic',
} as const
const ttsColletNames = { 'tts-er16': 'TTS / ER16', 'tts-er20': 'TTS / ER20', 'tts-er32': 'TTS / ER32' } as const

/** The holder's name in the panel's language. */
export function holderLabel(holder: ToolHolder): string {
  if (holder in holderNames) return t(holderNames[holder as keyof typeof holderNames])
  if (holder in ttsColletNames) return ttsColletNames[holder as keyof typeof ttsColletNames]
  return t('tools.holder.tts', { bore: holder.slice('tts-'.length) })
}

const er = {
  er16: { nut: 28, nutLength: 16, bodyLength: 38 },
  er20: { nut: 34, nutLength: 19, bodyLength: 42 },
  er32: { nut: 50, nutLength: 22, bodyLength: 48 },
} as const

interface HolderSurface {
  finish: 'ground' | 'dark'
  profile: [number, number][]
  wrenchFlats?: { face: number; radius: number; apothem: number; length: number }
}

/** Nominal outlines split at the ground and dark steel surfaces. */
export function holderSurfaces(holder: ToolHolder | undefined, face: number, shankDiameter: number): HolderSurface[] {
  if (holder === undefined || holder === 'r8') return []
  if (holder === 'generic') {
    const bore = shankDiameter / 2
    const radius = Math.max(10, bore + 4)
    return [{ finish: 'dark', profile: [[bore, face + 3], [bore, face], [radius - 1, face],
      [radius, face + 1], [radius, face + 22], [radius - 1, face + 23], [0, face + 23]] }]
  }
  if (holder === 'bt30') {
    const gauge = face + 33
    const bore = Math.min(shankDiameter / 2, 11)
    return [
      { finish: 'dark', profile: [[bore, face + 3], [bore, face], [12, face], [13, face + 1],
        [13, face + 12], [17, face + 17], [22, face + 17], [23, face + 18], [23, gauge - 10],
        [20, gauge - 8], [20, gauge - 6], [23, gauge - 4], [23, gauge - 1], [22, gauge], [15.875, gauge]] },
      { finish: 'ground', profile: [[15.875, gauge], [10.625, gauge + 36], [10, gauge + 37], [0, gauge + 37]] },
    ]
  }
  const shank = 19.05 / 2
  const collar = 31.75 / 2
  const ttsShoulder = (top: number): HolderSurface[] => [
    { finish: 'dark', profile: [[collar - 0.3, top - 6.4], [collar - 1.8, top - 6.4],
      [collar - 1.8, top - 4.6], [collar, top - 4.2]] },
    { finish: 'ground', profile: [[collar, top - 4.2], [collar, top - 0.4], [collar - 0.4, top]] },
    { finish: 'dark', profile: [[collar - 0.4, top], [collar - 1, top], [collar - 1.3, top - 1],
      [shank, top - 1], [shank, top + 2]] },
    { finish: 'ground', profile: [[shank, top + 2], [shank, top + 27], [shank - 2.3, top + 31.75],
      [5.5, top + 31.75]] },
    { finish: 'dark', profile: [[5.5, top + 31.75], [5.1, top + 31.35], [5.1, top + 29], [0, top + 29]] },
  ]
  const colletSize = holder === 'er16' || holder === 'tts-er16' ? 'er16'
    : holder === 'er20' || holder === 'tts-er20' ? 'er20'
      : holder === 'er32' || holder === 'tts-er32' ? 'er32' : null
  if (colletSize !== null) {
    const { nut, nutLength, bodyLength } = er[colletSize]
    const radius = nut / 2
    const bore = Math.min(shankDiameter / 2, radius - 1.5)
    const nutBack = face + nutLength
    const top = face + bodyLength
    const neck = Math.min(radius - 2, collar - 2.8)
    const collet = Math.max(bore, radius * 0.55)
    const tts = holder.startsWith('tts-')
    const body: HolderSurface[] = [
      { finish: 'ground', profile: [[bore, face + 1], [bore, face + 0.2], [collet, face + 0.2]] },
      { finish: 'dark', profile: [[collet, face + 0.2], [collet, face], [radius - 1.2, face],
        [radius, face + 1.3], [radius, face + nutLength * 0.57], [radius, face + nutLength * 0.78],
        [radius, nutBack - 0.9], [radius - 0.8, nutBack], [neck, nutBack]],
      wrenchFlats: { face, radius, apothem: Math.max(collet, radius * Math.sqrt(3) / 2), length: nutLength } },
    ]
    if (!tts) return [...body, { finish: 'ground', profile: [[neck, nutBack], [neck, top - 1],
      [neck - 1, top], [0, top]] }]
    return [...body, { finish: 'dark', profile: [[neck, nutBack], [neck, top - 13], [collar - 0.6, top - 11.2]] },
      { finish: 'ground', profile: [[collar - 0.6, top - 11.2], [collar, top - 10.6],
        [collar, top - 6.7], [collar - 0.3, top - 6.4]] },
      ...ttsShoulder(top),
    ]
  }
  const bore = Number(holder.slice('tts-'.length)) / 2
  const top = face + 30
  return [
    { finish: 'dark', profile: [[0, face + 2], [bore, face + 2], [bore, face], [12, face], [13.5, face + 2],
      [13.5, top - 9], [collar - 0.5, top - 9]] },
    { finish: 'ground', profile: [[collar - 0.5, top - 9], [collar, top - 8.5], [collar, top - 6.7],
      [collar - 0.3, top - 6.4]] },
    ...ttsShoulder(top),
  ]
}

export function holderProfile(holder: ToolHolder | undefined, face: number, shankDiameter: number): [number, number][] {
  return holderSurfaces(holder, face, shankDiameter).flatMap(({ profile }) => profile)
}
