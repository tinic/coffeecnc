import { describe, expect, it } from 'vitest'
import type { Tool } from '../src/protocol'
import { resolveToolModel } from '../src/tool-model'
import { decodeToolModelDocument, emptyToolModelDocument, matchingToolModelAssignments,
  decodeToolModels, maxToolModelAssignments, toolModelBinding, type ToolModelDocument } from '../src/tool-model-settings'

const tool: Tool = { number: 3, pocket: 7, diameter: 6.35, length: -50.8, comment: 'Quarter inch mill' }
function saved(): ToolModelDocument {
  return { ...emptyToolModelDocument(), assignments: [{
    tool: { number: 3, pocket: 7, diameter: 6.35, comment: tool.comment },
    family: 'end-mill', overrides: { stickout: 30 },
  }] }
}

function firstEntry(document: ToolModelDocument): ToolModelDocument['assignments'][number] {
  const entry = document.assignments[0]
  if (entry === undefined) throw new Error('Expected a saved assignment')
  return entry
}

describe('native tool model document', () => {
  it('validates snapshot identity, revision, availability and total encoded size', () => {
    const good = { type: 'tool_models', machine_id: 'G0759', revision: 0, availability: 'available', reason: '', document: saved() }
    expect(decodeToolModels(good)).toEqual(good)
    expect(decodeToolModels({ ...good, revision: Number.MAX_SAFE_INTEGER })).not.toBeNull()
    for (const patch of [{ machine_id: null }, { machine_id: '' }, { machine_id: '../G0759' },
      { machine_id: 'é' }, { machine_id: 'a'.repeat(65) }, { revision: -1 }, { revision: 0.5 },
      { revision: Number.MAX_SAFE_INTEGER + 1 }, { document: null }, { availability: 'unknown' },
      { reason: 'a'.repeat(1024 * 1024) }, { extra: true },
    ]) expect(decodeToolModels({ ...good, ...patch })).toBeNull()
    for (const availability of ['session_only', 'unavailable']) {
      const snapshot = { ...good, machine_id: null, availability, document: null }
      expect(decodeToolModels(snapshot)).toEqual(snapshot)
      expect(decodeToolModels({ ...snapshot, revision: 1 })).toBeNull()
      expect(decodeToolModels({ ...snapshot, document: saved() })).toBeNull()
    }
  })
  it('round trips structured bindings and makes independent copies', () => {
    const original = saved()
    const decoded = decodeToolModelDocument(JSON.parse(JSON.stringify(original)))
    expect(decoded).toEqual(original)
    if (decoded === null) throw new Error('Expected a document')
    decoded.defaults.drill.stickout = 90
    firstEntry(decoded).overrides.stickout = 42
    expect(original.defaults.drill.stickout).toBe(75)
    expect(firstEntry(original).overrides.stickout).toBe(30)
  })

  it('rejects incomplete, future, unknown and malformed documents instead of supplying saved defaults', () => {
    const good = saved()
    for (const value of [null, [], {}, { ...good, version: 2 }, { ...good, defaults: {} },
      { ...good, revision: 1 }, { ...good, assignments: {} },
      { ...good, defaults: { ...good.defaults, mystery: good.defaults.drill } },
      { ...good, defaults: { ...good.defaults, drill: { ...good.defaults.drill, diameter: NaN } } },
      { ...good, defaults: { ...good.defaults, drill: { ...good.defaults.drill, stickout: 1000 } } },
      { ...good, defaults: { ...good.defaults, drill: { ...good.defaults.drill, verified: true } } },
    ]) expect(decodeToolModelDocument(value)).toBeNull()
  })

  it('bounds the catalog and rejects duplicate numbers rather than choosing one silently', () => {
    const document = saved()
    const entry = firstEntry(document)
    document.assignments.push({ ...entry, family: 'drill' })
    expect(decodeToolModelDocument(document)).toBeNull()
    document.assignments = Array.from({ length: maxToolModelAssignments }, (_, index) => ({
      ...entry, tool: { ...entry.tool, number: index + 1 },
    }))
    expect(decodeToolModelDocument(document)?.assignments).toHaveLength(maxToolModelAssignments)
    document.assignments.push({ ...entry, tool: { ...entry.tool, number: maxToolModelAssignments + 1 } })
    expect(decodeToolModelDocument(document)).toBeNull()
  })

  it('limits encoded bytes too, including JSON escape expansion', () => {
    const document = saved()
    const entry = firstEntry(document)
    document.assignments = Array.from({ length: maxToolModelAssignments }, (_, index) => ({
      ...entry, tool: { ...entry.tool, number: index + 1, comment: '\u0001'.repeat(255) },
    }))
    expect(decodeToolModelDocument(document)).toBeNull()
  })

  it('validates binding integer limits, finite diameter and UTF-8 comment bytes', () => {
    expect(toolModelBinding({ ...tool, number: 2147483647, pocket: 0, diameter: 0, comment: 'é'.repeat(127) + 'a' })).not.toBeNull()
    for (const patch of [{ number: 0 }, { number: 2147483648 }, { number: 1.5 }, { pocket: -1 },
      { pocket: 2147483648 }, { diameter: Infinity }, { diameter: -1 },
      { comment: 'é'.repeat(128) }, { comment: 'a'.repeat(256) }, { comment: 'bad\nrow' }, { comment: 'bad\0row' },
    ]) expect(toolModelBinding({ ...tool, ...patch })).toBeNull()
  })

  it('accepts inheritance and valid partial overrides, but never a diameter override', () => {
    const document = saved()
    const entry = firstEntry(document)
    expect(decodeToolModelDocument({ ...document, assignments: [{ ...entry, overrides: {} }] })).not.toBeNull()
    for (const custom of [{ diameter: 8 }, { stickout: 0 }, { tipAngle: 180 }, { stickout: NaN }, { other: 1 }]) {
      expect(decodeToolModelDocument({ ...document, assignments: [{ ...entry, overrides: custom }] })).toBeNull()
    }
    expect(decodeToolModelDocument({ ...document, assignments: [{ ...entry, family: 'unknown' }] })).toBeNull()
  })

  it('keeps reported mm diameter exact and ignores only the Z offset when restoring', () => {
    const document = saved()
    const matches = matchingToolModelAssignments(document, [{ ...tool, length: 999 }])
    expect(matches.size).toBe(1)
    expect(resolveToolModel(tool, matches.get(3), document.defaults)?.dimensions.diameter).toBe(6.35)
    for (const patch of [{ number: 4 }, { pocket: 8 }, { diameter: 0.25 }, { comment: 'Replacement' }]) {
      expect(matchingToolModelAssignments(document, [{ ...tool, ...patch }]).size).toBe(0)
    }
  })

  it('withholds absent or changed rows without deleting saved assignments', () => {
    const document = saved()
    expect(matchingToolModelAssignments(document, []).size).toBe(0)
    expect(matchingToolModelAssignments(document, [{ ...tool, diameter: 8 }]).size).toBe(0)
    expect(document.assignments).toHaveLength(1)
    expect(matchingToolModelAssignments(document, [tool]).size).toBe(1)
  })
})
