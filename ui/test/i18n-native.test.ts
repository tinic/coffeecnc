// Every operator sentence the native backend and the panel store produce in English is known to
// the run-time translator (src/i18n/messages.ts). The test reads the C++ sources and store.ts as
// text, takes the sentences out of them and asks matchMessage() for each one, so a new or changed
// sentence fails here until it is catalogued in parts/messages (or parts/backend).
import { describe, expect, it } from 'vitest'

import programWatch from '../../backend/src/linuxcnc/src/program_watch.cpp?raw'
import { i18n, setLanguage } from '../src/i18n'
import { matchMessage, translateMessage, translateResult } from '../src/i18n/messages'

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
const adapter = import.meta.glob<string>([
  '../../backend/src/linuxcnc/src/nml_status.cpp',
  '../../backend/src/core/include/coffeecnc/core/controller_port.hpp',
  '../../backend/src/app/src/alarm_log.cpp',
  '../../backend/src/core/include/coffeecnc/core/jog_stop_report.hpp',
  '../../backend/src/linuxcnc/src/nml_port.cpp',
  '../../backend/src/linuxcnc/src/nml_panel_source.cpp',
  '../../backend/src/linuxcnc/src/hal_diagnostics.cpp',
  '../../backend/src/app/src/diagnostics_feed.cpp',
  '../../backend/src/linuxcnc/src/preview_engine.cpp',
  '../../backend/src/app/src/machine_data.cpp',
  '../../backend/src/app/src/backup_archive.cpp',
  '../../backend/src/app/src/restore_helper.cpp',
  '../../backend/src/app/src/operator_settings_store.cpp',
  '../../backend/src/app/src/offset_history.cpp',
  '../../backend/src/app/src/tool_models.cpp',
  '../../backend/src/app/src/tool_model_store.cpp',
  '../../backend/src/app/src/tool_model_json.hpp',
  '../../backend/src/app/include/coffeecnc/app/tool_model_store.hpp',
  '../../backend/src/app/src/program_store.cpp',
  '../../backend/src/app/src/program_edit.cpp',
], { query: '?raw', import: 'default', eager: true })
/** Panel modules whose English reaches the operator as run-time text (a result reason, an import error). */
const modules = import.meta.glob<string>(['../src/bridge.ts', '../src/step.worker.ts', '../src/step-model.ts'],
  { query: '?raw', import: 'default', eager: true })
// The first setLanguage() of a language imports its catalogs, which vitest transforms and the
// vue-i18n plugin compiles on demand: about 0.2 s alone and over 5 s on a loaded host (#267).
// Load them while the file is collected, outside every test's timeout; the tests then only switch.
await Promise.all((['de', 'fr', 'ja'] as const).map(setLanguage))
await setLanguage('en')

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

interface Literal { text: string; before: string; after: string }

const literalRefusal = /^(?:\s*,\s*\{\})*\s*,\s*RefusalMessage\{\s*"([^"]+)"/
const formattedRefusal = /^[^;]*?\),\s*RefusalMessage\{\s*"([^"]+)"\s*,\s*\{([\s\S]*?)\}\s*\}\);/
const namedRefusal = /\b(\w+_reason)(?:\s*,\s*\{\})*\s*,\s*RefusalMessage\{\s*"([^"]+)"/g

function keyedRefusal({ before, after }: Literal): { key: string; names: string[] } | null {
  const key = literalRefusal.exec(after)?.[1]
  if (key !== undefined) return { key, names: [] }
  if (!before.trimEnd().endsWith('std::format(')) return null
  const match = formattedRefusal.exec(after)
  if (match?.[1] === undefined || match[2] === undefined) return null
  const names = [...match[2].matchAll(/\{\s*"(\w+)"\s*,/g)].map((field) => field[1] ?? '')
  return { key: match[1], names }
}

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
    const after = source.slice(end + 1, end + 900)
    if (last !== undefined && source.slice(previousEnd, index).trim() === '') {
      last.text += text
      last.after = after
    } else literals.push({ text, before: source.slice(Math.max(0, index - 300), index), after })
    previousEnd = end + 1
    index = end + 1
  }
  return literals
}

/** The arguments after the format string, up to its call's closing parenthesis. */
function formatArguments(after: string): { args: string[]; rest: string } | null {
  const tokens = /"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|[()[\]{},]/g
  const args: string[] = []
  let depth = 0
  let start = 0
  for (const { 0: token, index } of after.matchAll(tokens)) {
    if (token === ',' && depth === 0) {
      const argument = after.slice(start, index).trim()
      if (argument !== '') args.push(argument)
      start = index + 1
    } else if (token === ')' && depth === 0) {
      const argument = after.slice(start, index).trim()
      if (argument !== '') args.push(argument)
      return { args, rest: after.slice(index + 1) }
    } else if (token === '(' || token === '[' || token === '{') depth++
    else if (token === ')' || token === ']' || token === '}') depth--
  }
  return null
}

/** The operator sentences of one C++ file. */
function nativeSentences(path: string, source: string): string[] {
  const sentences: string[] = []
  for (const literal of cppLiterals(source)) {
    const { text, before } = literal
    // A keyed refusal carries separate English fallback text for older panels.
    if (keyedRefusal(literal) !== null) continue
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

  it('catalogues the file reader reasons embedded in preview diagnostics', () => {
    const comments = programWatch.slice(programWatch.indexOf('auto comment_refusal('),
      programWatch.indexOf('auto bare_block('))
    const names = programWatch.slice(programWatch.indexOf('auto first_forbidden_line('),
      programWatch.indexOf('// The words of a block'))
    const phrases = cppLiterals(comments + names).map(({ text }) => text)
      .filter((text) => /^(?:a|an) /.test(text))
    expect(phrases.map((text) => matchMessage(text.replaceAll('{}', 'log'))?.key)).toEqual([
      'backend.previewEngine.stopPythonLine', 'backend.previewEngine.stopPythonComment',
      'backend.previewEngine.stopFileWritingComment', 'backend.previewEngine.stopOwordName',
    ])
  })

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

  it('keeps the native refusal keys, English fallbacks and argument names in sync', () => {
    for (const [path, source] of files) {
      const keys = [...source.matchAll(/RefusalMessage\{\s*"([^"]+)"/g)].map((match) => match[1])
      const found: string[] = []
      const literals = cppLiterals(source)
      function check(key: string, names: string[], text: string): void {
        found.push(key)
        expect(i18n.global.te(key, 'en'), key).toBe(true)
        const markers = names.map((name) => `{${name}}`)
        const params = Object.fromEntries(names.map((name, index) => [name, markers[index]]))
        const english = i18n.global.t(key as never, params, { locale: 'en' })
        let index = 0
        expect(english, key).toBe(text.replaceAll('{}', () => markers[index++] ?? '{}'))
        expect(index, key).toBe(names.length)
      }
      const reasons = new Map<string, string>()
      for (const literal of literals) {
        const name = /\bconstexpr auto (\w+_reason)\s*=\s*$/.exec(literal.before)?.[1]
        if (name !== undefined) reasons.set(name, literal.text)
        const refusal = keyedRefusal(literal)
        if (refusal !== null) check(refusal.key, refusal.names, literal.text)
      }
      for (const [, name, key] of source.matchAll(namedRefusal)) {
        if (name === undefined || key === undefined) continue
        const text = reasons.get(name)
        expect(text, name).toBeDefined()
        check(key, [], text ?? '')
      }
      expect(found.sort(), path).toEqual(keys.sort())
    }
  })

  it('keeps the allowlist to literals that exist', () => {
    const all = Object.values(native).join('\n')
    for (const text of english) expect(all).toContain(`"${text}"`)
  })

  it('keeps native message keys, fallback text and named arguments in sync', () => {
    const sources = { ...native, ...adapter }
    const constantsByFile = new Map<string, Map<string, string>>()
    for (const [path, source] of Object.entries(sources)) {
      const constants = new Map<string, string>()
      for (const literal of cppLiterals(source)) {
        const name = /\bconstexpr (?:auto|std::string_view) (\w+)\s*=\s*$/.exec(literal.before)?.[1]
        if (name !== undefined) constants.set(name, literal.text)
      }
      constantsByFile.set(path, constants)
    }

    function constant(path: string, name: string): string | undefined {
      const local = constantsByFile.get(path)?.get(name)
      if (local !== undefined) return local
      const stem = path.slice(path.lastIndexOf('/') + 1).replace(/\.[^.]+$/, '')
      // A class member can be declared in its header, as with ToolModelStore::busy_reason.
      for (const [file, constants] of constantsByFile) {
        if (file.endsWith(`/${stem}.hpp`) && constants.has(name)) return constants.get(name)
      }
      const matches = [...constantsByFile.values()].flatMap((constants) => {
        const match = constants.get(name)
        return match === undefined ? [] : [match]
      })
      return new Set(matches).size === 1 ? matches[0] : undefined
    }

    const value = /MessageKey\{\s*"([^"]+)"(?:,\s*\{([\s\S]*?)\}\s*\})?/g
    const argument = /\{\s*"(\w+)"\s*,/g
    const beforeKey = /^(?:\s*,\s*(?:\{\}|true|false))*\s*,\s*MessageKey\{/
    const formattedKey = /^\s*,[^;"]*?MessageKey\{/
    const named = /\b(?:std::string\((\w+)\)|(\w+))(?:\s*,\s*\{\})*\s*,\s*(MessageKey\{)/g

    function checkMatch(match: RegExpMatchArray, fallback: string, args: readonly (string | undefined)[]): void {
      const key = match[1] ?? ''
      const names = [...(match[2] ?? '').matchAll(argument)].map((arg) => arg[1] ?? '')
      const markers = names.map((name) => `{${name}}`)
      const params = Object.fromEntries(names.map((name, index) => [name, markers[index]]))
      expect(i18n.global.te(key, 'en'), key).toBe(true)
      let parameter = 0
      let argumentIndex = 0
      const formatted = fallback.replace(/\{(?::[^{}]*)?\}/g,
        () => args[argumentIndex++] ?? markers[parameter++] ?? '{}')
      expect(i18n.global.t(key as never, params, { locale: 'en' }), key).toBe(formatted)
      expect(parameter, key).toBe(names.length)
    }

    for (const [path, source] of Object.entries(sources)) {
      const expected = [...source.matchAll(value)].map((match) => match[1])
      const checked: (string | undefined)[] = []
      const check = (site: string, fallback: string, args: readonly (string | undefined)[] = []): void => {
        const match = [...site.matchAll(value)][0]
        if (match === undefined) return
        checkMatch(match, fallback, args)
        checked.push(match[1])
      }
      for (const literal of cppLiterals(source)) {
        if (beforeKey.test(literal.after)) check(literal.after, literal.text)
        else if (literal.before.trimEnd().endsWith('std::format(')) {
          const format = formatArguments(literal.after)
          if (format !== null && formattedKey.test(format.rest))
            check(format.rest, literal.text, format.args.map((arg) => constant(path, arg)))
        }
      }
      for (const match of source.matchAll(named)) {
        const fallback = constant(path, match[1] ?? match[2] ?? '')
        if (fallback === undefined) continue
        const from = match.index + match[0].lastIndexOf(match[3] ?? '')
        check(source.slice(from), fallback)
      }
      expect(checked.sort(), path).toEqual(expected.sort())
    }
  })
})

describe('translateMessage', () => {
  it('uses a structured key for a timeout without matching its English reason', async () => {
    await setLanguage('de')
    expect(translateResult({ type: 'result', id: 8, outcome: 'timed_out', reason: 'Older fallback.',
      localization: { key: 'messages.session.linuxcncNoAnswer', params: {} } })).toBe('LinuxCNC hat nicht geantwortet.')
  })
  it('translates a known refusal', async () => {
    await setLanguage('de')
    expect(translateMessage('Load a program first.')).toBe('Laden Sie zuerst ein Programm.')
    await setLanguage('ja')
    expect(translateMessage('Load a program first.')).toBe('先にプログラムを読み込んでください。')
  })

  it('uses a refusal key first and falls back when metadata is unknown or incomplete', async () => {
    await setLanguage('de')
    const fallback = 'The older panel can show this as-is.'
    expect(translateMessage(fallback, { key: 'messages.policy.jogVelocityAxisLimit', params: { axis: 'Z', limit: '750' } }))
      .toBe('Die Tippgeschwindigkeit liegt über der Grenze der Z-Achse von 750 mm/min.')
    expect(translateMessage(fallback, { key: 'messages.policy.jogVelocityAxisLimit', params: { axis: 'Z' } }))
      .toBe(fallback)
    expect(translateMessage('Load a program first.', { key: 'messages.future.newKey', params: {} }))
      .toBe('Laden Sie zuerst ein Programm.')
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

  it('keeps data placeholders verbatim, also when they are catalog words', async () => {
    await setLanguage('fr')
    // `two` is a catalog word ("deux"), but here it names a subroutine file.
    expect(translateMessage('No preview: two.ngc line 3: two.'))
      .toBe('Pas d’aperçu\u202f: two.ngc ligne 3\u202f: deux.')
    const detail = 'a (log...) comment writes a file'
    const expected = 'Pas d’aperçu\u202f: two.ngc ligne 3\u202f: un commentaire (log...) écrit un fichier.'
    expect(translateMessage(`No preview: two.ngc line 3: ${detail}.`)).toBe(expected)
    expect(translateMessage('Older fallback.', { key: 'backend.previewEngine.subroutineForbidden',
      params: { name: 'two', line: '3', detail } })).toBe(expected)
    expect(translateMessage('the directory two cannot be made: Permission denied'))
      .toBe('impossible de créer le dossier two\u202f: Permission denied')
    expect(translateMessage("Can't open home: no such file")).not.toContain('Référencement')
  })

  it('keeps operating system errors in HAL launch refusals verbatim', async () => {
    await setLanguage('de')
    const systemError = 'a value is not a finite number'
    expect(translateMessage(systemError)).not.toBe(systemError)
    const reason = `halscope could not be started: ${systemError}.`
    const expected = `halscope konnte nicht gestartet werden: ${systemError}.`
    expect(translateResult({ type: 'result', id: 50, outcome: 'rejected', reason,
      refusal: { key: 'backend.halDiagnostics.notStartedBecause', params: { name: 'halscope', system_error: systemError } } })).toBe(expected)
    expect(translateMessage(reason)).toBe(expected)
  })

  it('translates a nested reason', async () => {
    await setLanguage('fr')
    expect(translateMessage("Can't home: machine is in E-stop"))
      .toBe('Référencement impossible\u202f: la machine est en arrêt d’urgence')
    expect(translateMessage("Arc move in MDI would exceed X's negative limit"))
      .toBe('Le mouvement circulaire en MDI dépasserait la limite négative de X')
  })

  it('translates the phrases native fills into a sentence, and keeps its data', async () => {
    await setLanguage('de')
    // nml_port.cpp withdraw: the commands sent, joined by ", ", and the one not sent.
    expect(translateMessage('Cancelled after this was sent: the homing command, flood on. Not sent: mist on.'))
      .toBe('Abgebrochen, nachdem dies gesendet wurde: der Referenzfahrtbefehl, Flutkühlung ein. Nicht gesendet: Sprühnebel ein.')
    expect(translateMessage('Could not position XY for the setter (timed out).'))
      .toBe('XY konnte nicht für den Werkzeuglängensensor positioniert werden (Zeitüberschreitung).')
    // `latched` is the probe value: a number, not a phrase.
    expect(translateMessage('The probe did not release at the retract position (probe touching, latched 1, commanded +0.500 mm, measured +0.400 mm from the retract).'))
      .toBe('Der Messtaster war an der Rückzugsposition nicht frei (Taster berührt, Latch 1, Soll +0.500 mm, gemessen +0.400 mm vom Rückzug).')
    // tool_models.cpp: `object` is a phrase ("The file") or a JSON member name, kept as it is.
    expect(translateMessage('The file must be an object.')).toBe('Die Datei muss ein Objekt sein.')
    expect(translateMessage('document lacks the field version.')).toBe('document: Das Feld version fehlt.')
    await setLanguage('ja')
    expect(translateMessage('Cancelled after this was sent: the homing command, flood on. Not sent: mist on.'))
      .toBe('原点復帰コマンド、フラッドオン の送信後にキャンセルされました。未送信：ミストオン。')
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
