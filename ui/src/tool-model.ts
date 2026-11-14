import type { Tool } from './protocol'

export const toolFamilies = [
  { id: 'end-mill', name: 'Flat end mill' },
  { id: 'ball-mill', name: 'Ball-nose end mill' },
  { id: 'drill', name: 'Drill' },
  { id: 'spot-drill', name: 'Spot drill' },
  { id: 'chamfer', name: 'Chamfer mill' },
  { id: 'face-mill', name: 'Face mill' },
  { id: 'generic', name: 'Generic cylinder' },
] as const
export type ToolFamily = typeof toolFamilies[number]['id']

/** Physical envelope in mm; never LinuxCNC's tool-length (Z) offset. */
export interface ToolDimensions {
  diameter: number
  cuttingLength: number
  shankDiameter: number
  overallLength: number
  /** Distance from the tool tip to the holder face. The holder itself is not modeled. */
  stickout: number
  /** Included angle, degrees; used only by pointed tool families. */
  tipAngle: number
}
export type ToolDefaults = Record<ToolFamily, ToolDimensions>
export type ToolOverrides = Partial<Omit<ToolDimensions, 'diameter'>>
export interface ToolModelAssignment {
  identity: string
  family: ToolFamily
  overrides: ToolOverrides
}
export interface ToolModel {
  family: ToolFamily
  dimensions: ToolDimensions
  nominalDiameter: boolean
  customized: boolean
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
  }
}

export function pointedTool(family: ToolFamily): boolean {
  return family === 'drill' || family === 'spot-drill' || family === 'chamfer'
}

export function toolDimensionError(family: ToolFamily, d: ToolDimensions): string | null {
  const lengths = [d.diameter, d.cuttingLength, d.shankDiameter, d.overallLength, d.stickout]
  if (lengths.some((n) => !Number.isFinite(n) || n < 0.001 || n > 10_000)) return 'Dimensions must be between 0.001 and 10,000 mm.'
  if (!Number.isFinite(d.tipAngle) || d.tipAngle < 1 || d.tipAngle > 179) return 'Included tip angle must be between 1° and 179°.'
  if (d.cuttingLength > d.stickout || d.stickout > d.overallLength || d.cuttingLength >= d.overallLength) {
    return 'Cutting length must fit within stick-out, and stick-out within overall length. Leave some shank above the cutting section.'
  }
  if (family === 'ball-mill' && d.cuttingLength < d.diameter / 2) return 'Cutting length must include the full ball radius.'
  if (pointedTool(family) && d.diameter / 2 / Math.tan(d.tipAngle * Math.PI / 360) > d.cuttingLength) {
    return 'The pointed tip does not fit within the cutting length at this diameter and angle.'
  }
  return null
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
  const dimensions: ToolDimensions = {
    diameter, cuttingLength: preset.cuttingLength * scale, shankDiameter: preset.shankDiameter * scale,
    overallLength: preset.overallLength * scale, stickout: preset.stickout * scale, tipAngle: preset.tipAngle,
    ...assignment.overrides,
  }
  return { family: assignment.family, dimensions, nominalDiameter, customized: Object.keys(assignment.overrides).length > 0 }
}

/** Radius/height profiles. Origin is the cutting tip, body extends along +Z in mm.
 * These describe rotational envelopes: no flutes, inserts, holder or collision guarantees. */
export function toolProfiles(model: ToolModel): { cutting: [number, number][]; shank: [number, number][] } {
  const d = model.dimensions
  const error = toolDimensionError(model.family, d)
  if (error !== null) throw new Error(error)
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
  return { cutting, shank: [[0, d.cuttingLength], [d.shankDiameter / 2, d.cuttingLength],
    [d.shankDiameter / 2, d.overallLength], [0, d.overallLength]] }
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
      const fields = Object.keys(fallback[id]) as (keyof ToolDimensions)[]
      if (!fields.every((key) => typeof Reflect.get(candidate, key) === 'number')) continue
      const dimensions = Object.fromEntries(fields.map((key) => [key, Reflect.get(candidate, key)])) as unknown as ToolDimensions
      if (toolDimensionError(id, dimensions) === null) fallback[id] = dimensions
    }
  } catch { /* Ignore corrupt or unavailable preferences. */ }
  return fallback
}
