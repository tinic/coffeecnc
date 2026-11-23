import { t } from './i18n'
import type { Tool } from './protocol'
import { holderProfile, type ToolHolder } from './tool-holder'

export const toolFamilies = [
  { id: 'end-mill' }, { id: 'ball-mill' }, { id: 'drill' }, { id: 'spot-drill' },
  { id: 'chamfer' }, { id: 'face-mill' }, { id: 'generic' }, { id: 'touch-probe' },
] as const
export type ToolFamily = typeof toolFamilies[number]['id']

const familyNames = {
  'end-mill': 'tools.family.endMill', 'ball-mill': 'tools.family.ballMill', drill: 'tools.family.drill',
  'spot-drill': 'tools.family.spotDrill', chamfer: 'tools.family.chamfer', 'face-mill': 'tools.family.faceMill',
  generic: 'tools.family.generic', 'touch-probe': 'tools.family.touchProbe',
} as const

/** The family's name in the panel's language. */
export function toolFamilyName(family: ToolFamily): string {
  return t(familyNames[family])
}
type CutterFamily = Exclude<ToolFamily, 'touch-probe'>

/** Physical envelope in mm; never LinuxCNC's tool-length (Z) offset. */
export interface CutterDimensions {
  diameter: number
  cuttingLength: number
  shankDiameter: number
  overallLength: number
  /** Distance from the tool tip to the holder face. */
  stickout: number
  /** Included angle, degrees; used only by pointed tool families. */
  tipAngle: number
}
export interface ProbeDimensions {
  diameter: number
  shankDiameter: number
  bodyDiameter: number
  bodyLength: number
  /** From the top of the ball to the bottom of the housing. */
  stylusLength: number
}
export type ToolDimensions = CutterDimensions | ProbeDimensions
export type DimensionKey = keyof CutterDimensions | keyof ProbeDimensions
export type ToolDefaults = Record<CutterFamily, CutterDimensions> & { 'touch-probe': ProbeDimensions }
export type ToolOverrides = Partial<Record<Exclude<DimensionKey, 'diameter'>, number>>
export interface ToolModelAssignment {
  identity: string
  family: ToolFamily
  overrides: ToolOverrides
  holder?: ToolHolder
}
export interface ToolModel {
  family: ToolFamily
  dimensions: ToolDimensions
  nominalDiameter: boolean
  customized: boolean
  holder?: ToolHolder
}

/** Starting proportions, not manufacturer geometry or measured tool assemblies. */
export function factoryToolDefaults(): ToolDefaults {
  return {
    'end-mill': { diameter: 10, cuttingLength: 22, shankDiameter: 10, overallLength: 72, stickout: 35, tipAngle: 90 },
    'ball-mill': { diameter: 10, cuttingLength: 20, shankDiameter: 10, overallLength: 72, stickout: 35, tipAngle: 90 },
    drill: { diameter: 10, cuttingLength: 60, shankDiameter: 10, overallLength: 100, stickout: 75, tipAngle: 118 },
    'spot-drill': { diameter: 10, cuttingLength: 15, shankDiameter: 10, overallLength: 60, stickout: 30, tipAngle: 90 },
    chamfer: { diameter: 10, cuttingLength: 12, shankDiameter: 10, overallLength: 60, stickout: 30, tipAngle: 90 },
    'face-mill': { diameter: 50, cuttingLength: 12, shankDiameter: 20, overallLength: 70, stickout: 40, tipAngle: 90 },
    generic: { diameter: 10, cuttingLength: 20, shankDiameter: 10, overallLength: 70, stickout: 35, tipAngle: 90 },
    'touch-probe': { diameter: 4, shankDiameter: 12, bodyDiameter: 28, bodyLength: 32, stylusLength: 20 },
  }
}

export function pointedTool(family: ToolFamily): boolean {
  return family === 'drill' || family === 'spot-drill' || family === 'chamfer'
}

export function toolDimensionError(family: ToolFamily, d: ToolDimensions): string | null {
  if (family === 'touch-probe') {
    if (!('bodyDiameter' in d) || ![d.diameter, d.shankDiameter, d.bodyDiameter, d.bodyLength, d.stylusLength].every(validLength)) {
      return t('tools.model.dimensionRange')
    }
    return null
  }
  if (!('cuttingLength' in d)) return t('tools.model.cutterRequired')
  const lengths = [d.diameter, d.cuttingLength, d.shankDiameter, d.overallLength, d.stickout]
  if (!lengths.every(validLength)) return t('tools.model.dimensionRange')
  if (!Number.isFinite(d.tipAngle) || d.tipAngle < 1 || d.tipAngle > 179) return t('tools.model.tipAngleRange')
  if (d.cuttingLength > d.stickout || d.stickout > d.overallLength || d.cuttingLength >= d.overallLength) {
    return t('tools.model.lengthOrder')
  }
  if (family === 'ball-mill' && d.cuttingLength < d.diameter / 2) return t('tools.model.ballRadius')
  if (pointedTool(family) && d.diameter / 2 / Math.tan(d.tipAngle * Math.PI / 360) > d.cuttingLength) {
    return t('tools.model.pointedTip')
  }
  return null
}

function validLength(n: number): boolean { return Number.isFinite(n) && n >= 0.001 && n <= 10_000 }

export const cutterKeys = ['diameter', 'shankDiameter', 'cuttingLength', 'overallLength', 'stickout', 'tipAngle'] as const
export const probeKeys = ['diameter', 'shankDiameter', 'bodyDiameter', 'bodyLength', 'stylusLength'] as const
export function dimensionKeys(family: ToolFamily): readonly DimensionKey[] {
  return family === 'touch-probe' ? probeKeys : cutterKeys
}

export function readToolDimensions(family: ToolFamily, read: (key: DimensionKey) => number): ToolDimensions {
  const common = { diameter: read('diameter'), shankDiameter: read('shankDiameter') }
  if (family === 'touch-probe') return {
    ...common, bodyDiameter: read('bodyDiameter'), bodyLength: read('bodyLength'), stylusLength: read('stylusLength'),
  }
  return { ...common, cuttingLength: read('cuttingLength'), overallLength: read('overallLength'),
    stickout: read('stickout'), tipAngle: read('tipAngle') }
}

export function shapeOverrides(d: ToolDimensions): ToolOverrides {
  if ('bodyDiameter' in d) return {
    shankDiameter: d.shankDiameter, bodyDiameter: d.bodyDiameter, bodyLength: d.bodyLength, stylusLength: d.stylusLength,
  }
  return { shankDiameter: d.shankDiameter, cuttingLength: d.cuttingLength, overallLength: d.overallLength,
    stickout: d.stickout, tipAngle: d.tipAngle }
}

export function setToolDefault(defaults: ToolDefaults, family: ToolFamily, dimensions: ToolDimensions): void {
  if (family === 'touch-probe' && 'bodyDiameter' in dimensions) defaults[family] = dimensions
  else if (family !== 'touch-probe' && 'cuttingLength' in dimensions) defaults[family] = dimensions
}

/** A changed description, diameter or pocket invalidates the session's visual assignment.
 * Touch-off changes Z offset only and deliberately does not invalidate it. */
export function toolIdentity(tool: Tool): string {
  return JSON.stringify([tool.number, tool.pocket, tool.diameter, tool.comment])
}

/** Family lengths scale with the table diameter; explicit per-tool lengths are absolute mm. */
export function resolveToolModel(tool: Tool, assignment: ToolModelAssignment | undefined, defaults: ToolDefaults): ToolModel | null {
  if (assignment?.identity !== toolIdentity(tool)) return null
  const preset = defaults[assignment.family]
  const nominalDiameter = !Number.isFinite(tool.diameter) || tool.diameter <= 0
  const diameter = nominalDiameter ? preset.diameter : tool.diameter
  const scale = diameter / preset.diameter
  // Probe housings and styli do not scale when the replaceable ball diameter changes.
  const dimensions = readToolDimensions(assignment.family, (key) => {
    if (key === 'diameter') return diameter
    const nominal = Reflect.get(preset, key) as number
    return assignment.overrides[key] ?? nominal * (assignment.family === 'touch-probe' || key === 'tipAngle' ? 1 : scale)
  })
  return { family: assignment.family, dimensions, nominalDiameter, customized: Object.keys(assignment.overrides).length > 0,
    ...(assignment.holder === undefined ? {} : { holder: assignment.holder }) }
}

/** Radius/height profiles. Origin is the cutting tip, body extends along +Z in mm.
 * These describe the cutting section and exposed shank as rotational envelopes. */
export function toolProfiles(model: ToolModel): { cutting: [number, number][]; shank: [number, number][] } {
  const d = model.dimensions
  const error = toolDimensionError(model.family, d)
  if (error !== null) throw new Error(error)
  if (!('cuttingLength' in d)) throw new Error('The probe uses separate ball, stem, housing and shank geometry.')
  const radius = d.diameter / 2
  const cutting: [number, number][] = [[0, 0]]
  if (model.family === 'ball-mill') {
    for (let i = 1; i <= 24; i += 1) {
      const angle = Math.PI * i / 48
      cutting.push([radius * Math.sin(angle), radius * (1 - Math.cos(angle))])
    }
  } else if (pointedTool(model.family)) {
    cutting.push([radius, radius / Math.tan(d.tipAngle * Math.PI / 360)])
  } else cutting.push([radius, 0])
  cutting.push([radius, d.cuttingLength], [0, d.cuttingLength])
  const shankEnd = model.holder === undefined ? d.overallLength : d.stickout
  return { cutting, shank: shankEnd > d.cuttingLength ? shankProfile(d.shankDiameter / 2, d.cuttingLength, shankEnd) : [] }
}

export function shankProfile(radius: number, start: number, end: number): [number, number][] {
  const bevel = Math.min(radius * 0.08, (end - start) * 0.18, 0.55)
  return [[0, start], [radius, start], [radius, end - bevel], [radius - bevel, end], [0, end]]
}

/** Dimensions from the ball bottom, which is also the model's Z datum. */
export function probeEnvelope(d: ProbeDimensions): { bodyStart: number; bodyEnd: number; overallLength: number; stylusDiameter: number } {
  const bodyStart = d.diameter + d.stylusLength
  const bodyEnd = bodyStart + d.bodyLength
  return { bodyStart, bodyEnd, overallLength: bodyEnd + d.shankDiameter, stylusDiameter: Math.min(d.diameter * 0.45, d.bodyDiameter * 0.25) }
}

export function holderFaceZ(d: ToolDimensions): number {
  return 'bodyDiameter' in d ? probeEnvelope(d).bodyEnd : d.stickout
}

export function toolEnvelope(d: ToolDimensions, holder?: ToolHolder): { diameter: number; overallLength: number } {
  const probe = 'bodyDiameter' in d
  const face = holderFaceZ(d)
  const profile = holderProfile(holder, face, d.shankDiameter)
  const exposedLength = holder === undefined ? (probe ? probeEnvelope(d).overallLength : d.overallLength) : face
  return {
    diameter: Math.max(d.diameter, d.shankDiameter, probe ? d.bodyDiameter : 0, ...profile.map(([r]) => r * 2)),
    overallLength: Math.max(exposedLength, ...profile.map(([, z]) => z)),
  }
}

/** Persisted preferences are untrusted, versioned, and validated before use. */
export function decodeToolDefaults(text: string | null): ToolDefaults {
  const fallback = factoryToolDefaults()
  if (text === null) return fallback
  try {
    const value: unknown = JSON.parse(text)
    if (typeof value !== 'object' || value === null || !('version' in value) || value.version !== 1 || !('defaults' in value)) return fallback
    const defaults = value.defaults
    if (typeof defaults !== 'object' || defaults === null) return fallback
    for (const { id } of toolFamilies) {
      const candidate: unknown = Reflect.get(defaults, id)
      if (typeof candidate !== 'object' || candidate === null) continue
      const fields = dimensionKeys(id)
      if (!fields.every((key) => typeof Reflect.get(candidate, key) === 'number')) continue
      const dimensions = readToolDimensions(id, (key) => Reflect.get(candidate, key) as number)
      if (toolDimensionError(id, dimensions) === null) setToolDefault(fallback, id, dimensions)
    }
  } catch { /* Ignore corrupt or unavailable preferences. */ }
  return fallback
}
