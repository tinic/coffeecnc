import type { Status, Tool, Vec3 } from './protocol'
import { resolveToolModel, toolDimensionError, toolEnvelope, type ToolDefaults, type ToolModel, type ToolModelAssignment } from './tool-model'

export interface ToolPreview { number: number; tip: Vec3; model: ToolModel }

/** The same tool-tip coordinates used by PreviewSegment; apply the active offset once. */
// Without required homing (the G0759) machine coordinates hold for the session, like the program's
// own frame, so the tool is placed against the path all the same.
export function reportedToolTip(status: Status | null, homingRequired = true): Vec3 | null {
  if (status === null) return null
  const axes = (['x', 'y', 'z'] as const).map((letter) => status.axes.find((axis) => axis.axis === letter))
  if (axes.some((axis) => axis === undefined || (homingRequired && !axis.homed) || axis.homing ||
    !Number.isFinite(axis.machine)) ||
    !status.offsets.tool.every(Number.isFinite)) return null
  return axes.map((axis, index) => (axis?.machine ?? 0) - (status.offsets.tool[index] ?? 0)) as Vec3
}

/** Only an explicitly assigned, currently loaded tool may supply a body. */
export function loadedToolPreview(status: Status | null, live: boolean, tools: Tool[],
  assignments: ReadonlyMap<number, ToolModelAssignment>, defaults: ToolDefaults,
  homingRequired = true): ToolPreview | null {
  if (!live || status?.link !== 'live' || status.wait.kind === 'tool_change') return null
  const tip = reportedToolTip(status, homingRequired)
  const tool = tools.find((candidate) => candidate.number === status.tool.in_spindle && candidate.number > 0)
  if (tip === null || tool === undefined) return null
  const model = resolveToolModel(tool, assignments.get(tool.number), defaults)
  if (model === null || toolDimensionError(model.family, model.dimensions) !== null) return null
  return { number: tool.number, tip, model }
}

export function toolPreviewBounds(preview: ToolPreview): { min: Vec3; max: Vec3 } {
  const { dimensions: d } = preview.model
  const { diameter, overallLength } = toolEnvelope(d, preview.model.holder)
  const radius = 'bodyDiameter' in d ? diameter / 2 : Math.max(diameter / 2, d.diameter * 0.6, d.shankDiameter * 0.7)
  const [x, y, z] = preview.tip
  return { min: [x - radius, y - radius, z], max: [x + radius, y + radius, z + overallLength] }
}
