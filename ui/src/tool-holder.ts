// Dimensions are nominal envelopes in millimetres. TTS uses a 3/4 in. shank.
// ER nut diameters follow the standard REGO-FIX envelopes (28, 34 and 50 mm).
const bores = [4, 5, 6, 7, 8, 9, 10, 11, 12] as const
export const holderChoices = [
  { id: 'r8', label: 'R8 collet' },
  ...bores.map((bore) => ({ id: `tts-${String(bore)}` as `tts-${typeof bores[number]}`, label: `TTS end mill · ${String(bore)} mm` })),
  { id: 'tts-er16', label: 'TTS / ER16' },
  { id: 'tts-er20', label: 'TTS / ER20' },
  { id: 'tts-er32', label: 'TTS / ER32' },
] as const
export type ToolHolder = typeof holderChoices[number]['id']

export function isToolHolder(value: unknown): value is ToolHolder {
  return holderChoices.some(({ id }) => id === value)
}

export function holderLabel(holder: ToolHolder): string {
  return holderChoices.find(({ id }) => id === holder)?.label ?? ''
}

const er = {
  'tts-er16': { nut: 28, nutLength: 16, bodyLength: 38 },
  'tts-er20': { nut: 34, nutLength: 19, bodyLength: 42 },
  'tts-er32': { nut: 50, nutLength: 22, bodyLength: 48 },
} as const

/** The radius/height section starts at the face where the cutter emerges. */
export function holderProfile(holder: ToolHolder | undefined, face: number, shankDiameter: number): [number, number][] {
  if (holder === undefined || holder === 'r8') return []
  const shank = 19.05 / 2
  const collar = 31.75 / 2
  const shankLength = 31.75
  if (holder === 'tts-er16' || holder === 'tts-er20' || holder === 'tts-er32') {
    const { nut, nutLength, bodyLength } = er[holder]
    const radius = nut / 2
    const bore = Math.min(shankDiameter / 2, radius - 1)
    return [[bore, face], [radius - 1.5, face], [radius, face + 1.5],
      [radius, face + nutLength], [radius - 2, face + nutLength],
      [collar, face + bodyLength - 5], [collar, face + bodyLength],
      [shank, face + bodyLength], [shank, face + bodyLength + shankLength],
      [0, face + bodyLength + shankLength], [0, face + 2], [bore, face + 2], [bore, face]]
  }
  const bore = Number(holder.slice('tts-'.length)) / 2
  const bodyLength = 30
  return [[bore, face], [12, face], [13.5, face + 2], [13.5, face + bodyLength - 7],
    [collar, face + bodyLength - 7], [collar, face + bodyLength],
    [shank, face + bodyLength], [shank, face + bodyLength + shankLength],
    [0, face + bodyLength + shankLength], [0, face + 2], [bore, face + 2], [bore, face]]
}
