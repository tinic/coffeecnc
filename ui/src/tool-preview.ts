import type { Status, Tool, Vec3 } from './protocol'
import { resolveToolModel, toolDimensionError, type ToolDefaults, type ToolModel, type ToolModelAssignment } from './tool-model'

export interface ToolPreview { number: number; tip: Vec3; model: ToolModel }

/** The same tool-tip coordinates used by PreviewSegment; apply the active offset once. */
export function reportedToolTip(status: Status | null): Vec3 | null {
  if (status === null) return null
  const axes = (['x', 'y', 'z'] as const).map((letter) => status.axes.find((axis) => axis.axis === letter))
  if (axes.some((axis) => axis === undefined || !axis.homed || axis.homing || !Number.isFinite(axis.machine)) ||
    !status.offsets.tool.every(Number.isFinite)) return null
  return axes.map((axis, index) => (axis?.machine ?? 0) - (status.offsets.tool[index] ?? 0)) as Vec3
}

/** Only an explicitly assigned, currently loaded tool may supply a body. */
export function loadedToolPreview(status: Status | null, live: boolean, tools: Tool[],
  assignments: ReadonlyMap<number, ToolModelAssignment>, defaults: ToolDefaults): ToolPreview | null {
  if (!live || status?.link !== 'live' || status.wait.kind === 'tool_change') return null
  const tip = reportedToolTip(status)
  const tool = tools.find((candidate) => candidate.number === status.tool.in_spindle && candidate.number > 0)
  if (tip === null || tool === undefined) return null
  const model = resolveToolModel(tool, assignments.get(tool.number), defaults)
  if (model === null || toolDimensionError(model.family, model.dimensions) !== null) return null
  return { number: tool.number, tip, model }
}

export function toolPreviewBounds(preview: ToolPreview): { min: Vec3; max: Vec3 } {
  const { dimensions: d } = preview.model
  const radius = Math.max(d.diameter * 0.6, d.shankDiameter * 0.7)
  const [x, y, z] = preview.tip
  return { min: [x - radius, y - radius, z], max: [x + radius, y + radius, z + d.overallLength] }
}
