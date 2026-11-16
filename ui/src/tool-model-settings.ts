import type { Tool, ToolModels } from './protocol'
import { cutterKeys, dimensionKeys, factoryToolDefaults, probeKeys, readToolDimensions, setToolDefault, toolDimensionError, toolFamilies, toolIdentity,
  type ToolDefaults, type ToolDimensions, type ToolFamily, type ToolModelAssignment, type ToolOverrides } from './tool-model'

export const maxToolModelBytes = 1024 * 1024
export const maxToolModelAssignments = 1001
const maxToolInteger = 2147483647
const familyIds = toolFamilies.map(({ id }) => id)
const legacyFamilies = familyIds.filter((id) => id !== 'touch-probe')

/** Exact reported values: diameter is already mm; Z is deliberately not identity. */
export type ToolModelBinding = Pick<Tool, 'number' | 'pocket' | 'diameter' | 'comment'>
export interface SavedToolModelAssignment {
  tool: ToolModelBinding
  family: ToolFamily
  overrides: ToolOverrides
}
export interface ToolModelDocument {
  version: 1
  defaults: ToolDefaults
  assignments: SavedToolModelAssignment[]
}

function record(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value)
}

/** Visit at most the permitted fields; do not allocate a list of arbitrary input keys. */
function fields(value: unknown, required: readonly string[], optional: readonly string[] = []): value is Record<string, unknown> {
  if (!record(value)) return false
  for (const key in value) {
    if (Object.hasOwn(value, key) && !required.includes(key) && !optional.includes(key)) return false
  }
  return required.every((key) => Object.hasOwn(value, key))
}

function family(value: unknown): value is ToolFamily {
  return typeof value === 'string' && familyIds.some((id) => id === value)
}

function dimension(value: unknown, angle: boolean): value is number {
  return typeof value === 'number' && Number.isFinite(value) &&
    value >= (angle ? 1 : 0.001) && value <= (angle ? 179 : 10000)
}

function dimensions(value: unknown, kind: ToolFamily): ToolDimensions | null {
  const keys = dimensionKeys(kind)
  if (!fields(value, keys) || !keys.every((key) => dimension(value[key], key === 'tipAngle'))) return null
  const copy = readToolDimensions(kind, (key) => Number(value[key]))
  return toolDimensionError(kind, copy) === null ? copy : null
}

function overrides(value: unknown, kind: ToolFamily): ToolOverrides | null {
  const overrideKeys = (kind === 'touch-probe' ? probeKeys : cutterKeys).filter((key) => key !== 'diameter')
  if (!fields(value, [], overrideKeys)) return null
  const copy: ToolOverrides = {}
  for (const key of overrideKeys) {
    if (!Object.hasOwn(value, key)) continue
    const n = value[key]
    if (!dimension(n, key === 'tipAngle')) return null
    copy[key] = n
  }
  return copy
}

function binding(value: unknown): ToolModelBinding | null {
  if (!fields(value, ['number', 'pocket', 'diameter', 'comment'])) return null
  const { number, pocket, diameter, comment } = value
  if (typeof number !== 'number' || !Number.isInteger(number) || number < 1 || number > maxToolInteger ||
    typeof pocket !== 'number' || !Number.isInteger(pocket) || pocket < 0 || pocket > maxToolInteger ||
    typeof diameter !== 'number' || !Number.isFinite(diameter) || diameter < 0 ||
    typeof comment !== 'string' || comment.length > 255 || /[\0\r\n]/u.test(comment) ||
    new TextEncoder().encode(comment).byteLength > 255) return null
  return { number, pocket, diameter, comment }
}

export function toolModelBinding(tool: Tool): ToolModelBinding | null {
  return binding({ number: tool.number, pocket: tool.pocket, diameter: tool.diameter, comment: tool.comment })
}

export function emptyToolModelDocument(): ToolModelDocument {
  return { version: 1, defaults: factoryToolDefaults(), assignments: [] }
}

export function validMachineId(value: unknown): value is string {
  return typeof value === 'string' && /^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$/u.test(value)
}

/** Validate before a snapshot can enable writes or supply a revision for compare-and-swap. */
export function decodeToolModels(value: unknown): ToolModels | null {
  if (!fields(value, ['type', 'machine_id', 'revision', 'availability', 'reason', 'document'])) return null
  const { type, machine_id, revision, availability, reason, document } = value
  if (type !== 'tool_models' || !(machine_id === null || validMachineId(machine_id)) ||
    typeof revision !== 'number' || !Number.isSafeInteger(revision) || revision < 0 ||
    typeof reason !== 'string' || reason.length > maxToolModelBytes) return null
  let decoded: ToolModels
  if (availability === 'available') {
    const doc = decodeToolModelDocument(document)
    if (machine_id === null || doc === null) return null
    decoded = { type, machine_id, revision, availability, reason, document: doc }
  } else {
    if ((availability !== 'session_only' && availability !== 'unavailable') || revision !== 0 || document !== null) return null
    decoded = { type, machine_id, revision, availability, reason, document }
  }
  return new TextEncoder().encode(JSON.stringify(decoded)).byteLength <= maxToolModelBytes ? decoded : null
}

/** Strict native documents are all-or-nothing; corrupt input is not an empty saved catalog. */
export function decodeToolModelDocument(value: unknown): ToolModelDocument | null {
  if (!fields(value, ['version', 'defaults', 'assignments']) || value['version'] !== 1 ||
    !fields(value['defaults'], legacyFamilies, ['touch-probe']) || !Array.isArray(value['assignments']) ||
    value['assignments'].length > maxToolModelAssignments) return null
  const defaults = factoryToolDefaults()
  for (const id of familyIds) {
    if (id === 'touch-probe' && !Object.hasOwn(value['defaults'], id)) continue
    const d = dimensions(value['defaults'][id], id)
    if (d === null) return null
    setToolDefault(defaults, id, d)
  }
  const assignments: SavedToolModelAssignment[] = []
  const used = new Set<number>()
  for (const entry of value['assignments']) {
    if (!fields(entry, ['tool', 'family', 'overrides']) || !family(entry['family'])) return null
    const tool = binding(entry['tool'])
    const custom = overrides(entry['overrides'], entry['family'])
    if (tool === null || custom === null || used.has(tool.number)) return null
    used.add(tool.number)
    assignments.push({ tool, family: entry['family'], overrides: custom })
  }
  const document: ToolModelDocument = { version: 1, defaults, assignments }
  // Structural bounds above make this serialization bounded, even before the byte check.
  return new TextEncoder().encode(JSON.stringify(document)).byteLength <= maxToolModelBytes ? document : null
}

export function toolModelBindingMatches(saved: ToolModelBinding, tool: Tool): boolean {
  return saved.number === tool.number && saved.pocket === tool.pocket &&
    saved.diameter === tool.diameter && saved.comment === tool.comment
}

/** A view of the saved catalog. Missing or changed rows never mutate the document. */
export function matchingToolModelAssignments(document: ToolModelDocument, tools: readonly Tool[]): Map<number, ToolModelAssignment> {
  const table = new Map(tools.map((tool) => [tool.number, tool]))
  const matches = new Map<number, ToolModelAssignment>()
  for (const saved of document.assignments) {
    const tool = table.get(saved.tool.number)
    if (tool !== undefined && toolModelBindingMatches(saved.tool, tool)) {
      matches.set(tool.number, { identity: toolIdentity(tool), family: saved.family, overrides: { ...saved.overrides } })
    }
  }
  return matches
}
