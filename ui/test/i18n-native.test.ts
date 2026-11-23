// Every operator sentence the native backend and the panel store produce in English is known to
// the run-time translator (src/i18n/messages.ts). The test reads the C++ sources and store.ts as
// text, takes the sentences out of them and asks matchMessage() for each one, so a new or changed
// sentence fails here until it is catalogued in parts/messages (or parts/backend).
import { describe, expect, it } from 'vitest'

import { setLanguage } from '../src/i18n'
import { matchMessage, translateMessage } from '../src/i18n/messages'

const native = import.meta.glob<string>([
  '../../backend/src/core/src/command_policy.cpp',
  '../../backend/src/app/src/session.cpp',
  '../../backend/src/app/src/request.cpp',
  '../../backend/src/app/src/response.cpp',
  '../../backend/src/app/src/messages.cpp',
  '../../backend/src/app/src/panel_feed.cpp',
  '../../backend/src/app/include/coffeecnc/app/session.hpp',
  '../../backend/src/app/include/coffeecnc/app/panel_feed.hpp',
], { query: '?raw', import: 'default', eager: true })
/** Panel modules whose English reaches the operator as run-time text (a result reason, an import error). */
const modules = import.meta.glob<string>(['../src/bridge.ts', '../src/step.worker.ts', '../src/step-model.ts'],
  { query: '?raw', import: 'default', eager: true })
const store = Object.values(import.meta.glob<string>('../src/store.ts', { query: '?raw', import: 'default', eager: true }))[0] ?? ''

/** Stands for a variable part: a placeholder in the catalog matches any text. */
const sample = 'X'
/** Stands for text appended to a literal that ends in ": " or ". " (a field name, a reason). */
const detail = 'Sample detail.'

/**
 * Literals that are not sentences on their own: the parts of a sentence the code composes, each
 * with the composed sentence the operator sees.
 */
const composed: Record<string, string> = {
  // session.cpp: "The previous jog stop on " + letter + " is not confirmed: " + reason.
  'The previous jog stop on ': `The previous jog stop on X is not confirmed: ${detail}`,
  ' is not confirmed: ': `The previous jog stop on X is not confirmed: ${detail}`,
  // session.cpp: "Jog stop failed on " + letter + ": " + reason.
  'Jog stop failed on ': `Jog stop failed on X: ${detail}`,
  // session.cpp: a refusal of its own, without a final period.
  'A touch probe is loaded': 'A touch probe is loaded',
}

/**
 * Sentences deliberately left English, with the reason. They never reach the operator as panel
 * text, or they are developer diagnostics.
 */
const english = new Set<string>([
  // command_policy.cpp: std::invalid_argument at start-up for an invalid INI; not a command result.
  'Invalid machine command limits',
])

interface Literal { text: string; before: string }

/** The string literals of C++ source, adjacent ones joined, comments skipped. */
function cppLiterals(source: string): Literal[] {
  const literals: Literal[] = []
  let index = 0
  let previousEnd = -1
  while (index < source.length) {
    const c = source[index]
    if (source.startsWith('//', index)) { index = source.indexOf('\n', index); if (index < 0) break; continue }
    if (source.startsWith('/*', index)) { index = source.indexOf('*/', index) + 2; continue }
    if (c === "'") { index = source.indexOf("'", index + (source[index + 1] === '\\' ? 3 : 2)) + 1; continue }
    if (c !== '"') { index += 1; continue }
    let text = ''
    let end = index + 1
    while (end < source.length && source[end] !== '"') {
      if (source[end] === '\\') { text += source[end + 1] === 'n' ? '\n' : source[end + 1] ?? ''; end += 2; continue }
      text += source.charAt(end)
      end += 1
    }
    const last = literals.at(-1)
    if (last !== undefined && source.slice(previousEnd, index).trim() === '') last.text += text
    else literals.push({ text, before: source.slice(Math.max(0, index - 300), index) })
    previousEnd = end + 1
    index = end + 1
  }
  return literals
}

/** The operator sentences of one C++ file. */
function nativeSentences(path: string, source: string): string[] {
  const sentences: string[] = []
  for (const { text, before } of cppLiterals(source)) {
    // Compile-time diagnostics.
    if (/static_assert\([^;"]*$/.test(before)) continue
    // Operator text starts with a capital and has words; G-code, field names and wire values do not.
    if (!/^[A-Z]/.test(text) || !/[a-z]{2,}[ ,;:]+[a-z]{2,}/.test(text)) continue
    if (english.has(text)) continue
    const filled = before.trimEnd().endsWith('std::format(') ?text.replace(/\{[^}]*\}/g, sample) : text
    if (/[.?!]$/.test(filled)) sentences.push(filled)
    else if (/[:.] $/.test(filled)) sentences.push(filled + detail)
    else sentences.push(composed[filled] ?? `${path}: no composed sentence for "${filled}"`)
  }
  for (const literal of Object.keys(composed).filter((key) => key.startsWith(' ')))
    if (source.includes(`"${literal}"`)) sentences.push(composed[literal] ?? literal)
  // messages.cpp: Fields::fail(field, problem) reports "<field> <problem>.".
  for (const match of source.matchAll(/\bfail\([^,()]+,\s*"([^"]+)"\)/g)) sentences.push(`max_jog_velocity ${match[1] ?? ''}.`)
  return sentences
}

/** A TypeScript string or template literal with each `${...}` filled in. */
function fill(literal: string, values: Record<string, string> = {}): string {
  return literal.replace(/\$\{([^}]*)\}/g, (_, expression: string) => values[expression.trim()] ?? sample)
}

function body(source: string, start: string, end: string): string {
  const from = source.indexOf(start)
  return source.slice(from, source.indexOf(end, from + start.length))
}

/** The store's log texts, result reasons, outcome words and command contexts. */
function storeSentences(source: string): string[] {
  const sentences: string[] = []
  // describeCommand: every capitalized literal is a context shown with the log entry.
  const values = { 'command.direction': 'forward', 'command.which': 'feed' }
  for (const match of body(source, 'export function describeCommand', '\nfunction ').matchAll(/'([A-Z][^']*)'|`([A-Z][^`]*)`/g))
    sentences.push(fill(match[1] ?? match[2] ?? '', values))
  for (const match of body(source, 'function outcomeWords', '\n}').matchAll(/return '([^']+)'/g)) sentences.push(match[1] ?? '')
  const panel = body(source, 'export function createPanel', '\n}\n')
  for (const pattern of [/const reason = '([^']+)'/g, /reason: '([^']+)'/g, /protocolError = `([^`]+)`/g, /log\('(?:error|info)', [^,]+, '([^']+)'/g])
    for (const match of panel.matchAll(pattern)) sentences.push(fill(match[1] ?? ''))
  return sentences
}

describe('native and store sentences', () => {
  const files = Object.entries(native)

  it('reads every source file', () => {
    expect(files.length).toBe(8)
    expect(store).toContain('export function describeCommand')
  })

  for (const [path, source] of files) {
    const name = path.slice(path.lastIndexOf('/') + 1)
    it(`catalogues every sentence of ${name}`, () => {
      const sentences = nativeSentences(name, source)
      expect(sentences.length).toBeGreaterThan(0)
      expect(sentences.filter((sentence) => matchMessage(sentence) === null)).toEqual([])
    })
  }

  it('catalogues every sentence of store.ts', () => {
    const sentences = storeSentences(store)
    expect(sentences.length).toBeGreaterThan(50)
    expect(sentences.filter((sentence) => matchMessage(sentence) === null)).toEqual([])
  })

  it('catalogues the result reasons and STEP import errors of the panel modules', () => {
    expect(Object.keys(modules).length).toBe(3)
    const sentences = Object.values(modules).flatMap((source) =>
      [...source.matchAll(/reason: '([^']+)'|new Error\('([^']+)'\)|error\.message : '([^']+)'/g)]
        .map((match) => match[1] ?? match[2] ?? match[3] ?? ''))
    expect(sentences.length).toBeGreaterThan(10)
    expect(sentences.filter((sentence) => matchMessage(sentence) === null)).toEqual([])
  })

  it('keeps the allowlist to literals that exist', () => {
    const all = Object.values(native).join('\n')
    for (const text of english) expect(all).toContain(`"${text}"`)
  })
})

describe('translateMessage', () => {
  it('translates a known refusal', async () => {
    await setLanguage('de')
    expect(translateMessage('Load a program first.')).toBe('Laden Sie zuerst ein Programm.')
    await setLanguage('ja')
    expect(translateMessage('Load a program first.')).toBe('先にプログラムを読み込んでください。')
  })

  it('keeps the numbers of a parameterised refusal', async () => {
    await setLanguage('de')
    expect(translateMessage('Jog velocity is above the X axis limit of 1500 mm/min.'))
      .toBe('Die Tippgeschwindigkeit liegt über der Grenze der X-Achse von 1500 mm/min.')
    await setLanguage('ja')
    expect(translateMessage('Home the machine before jogging Y: two motors drive it.'))
      .toBe('Y をジョグする前に機械を原点復帰してください：この軸は2台のモーターで駆動されています。')
  })

  it('translates the known parts of a composed sentence', async () => {
    await setLanguage('de')
    expect(translateMessage('Panel request ignored: Missing required field: axis.'))
      .toBe('Anfrage der Bedienoberfläche ignoriert: Pflichtfeld fehlt: axis.')
    // LinuxCNC's own text inside a sentence of ours stays as LinuxCNC sent it.
    expect(translateMessage('Saved on disk, but not reopened. emcTaskPlanOpen failed'))
      .toBe('Auf dem Datenträger gespeichert, aber nicht erneut geöffnet. emcTaskPlanOpen failed')
  })

  it('passes unknown text unchanged', async () => {
    await setLanguage('fr')
    expect(translateMessage('joint 0 following error')).toBe('joint 0 following error')
    expect(translateMessage('')).toBe('')
  })

  it('shows English text as it is in English', () => {
    expect(translateMessage('Load a program first.')).toBe('Load a program first.')
  })
})
