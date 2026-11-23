// Every operator sentence the native backend builds in the files below is catalogued in
// parts/backend (or parts/messages): src/i18n/messages.ts finds it, so the panel can translate
// it. The test reads the C++ sources, takes each string literal that reads as words (adjacent
// literals joined, `"a" + x + "b"` chains joined, std::format placeholders and other operands
// replaced by a sample value or by the string constant they name) and asks matchMessage for it.
// A new or changed sentence fails here until it is catalogued, or listed below as not operator
// text. LinuxCNC's own operator errors are not in these files: they are shown as LinuxCNC sends them.
//
// Not read, because their text reaches only stderr or the terminal, never the panel:
// machine_config.cpp (INI errors end the start), hal_component.cpp (HAL setup errors end the
// start), config_check.cpp and application.cpp, and the shell test hooks.
import { describe, expect, it } from 'vitest'

import { matchMessage } from '../src/i18n/messages'
// Raw: the vue-i18n plugin compiles a plain JSON import into message functions.
import backendCatalog from '../src/i18n/parts/backend/en.json?raw'
import messagesCatalog from '../src/i18n/parts/messages/en.json?raw'

const sources = import.meta.glob<string>([
  '../../backend/src/app/src/operator_settings.cpp',
  '../../backend/src/app/src/operator_settings_store.cpp',
  '../../backend/src/app/src/tool_models.cpp',
  '../../backend/src/app/src/tool_model_store.cpp',
  '../../backend/src/app/include/coffeecnc/app/tool_model_store.hpp',
  '../../backend/src/app/src/offset_history.cpp',
  '../../backend/src/app/src/machine_data.cpp',
  '../../backend/src/app/src/backup_archive.cpp',
  '../../backend/src/app/src/program_store.cpp',
  '../../backend/src/app/src/program_edit.cpp',
  '../../backend/src/app/src/file_system.cpp',
  '../../backend/src/app/src/preview.cpp',
  '../../backend/src/app/src/status_publisher.cpp',
  '../../backend/src/app/src/runtime.cpp',
  '../../backend/src/linuxcnc/src/nml_port.cpp',
  '../../backend/src/linuxcnc/src/preview_engine.cpp',
  '../../backend/src/linuxcnc/src/nml_status.cpp',
  '../../backend/src/linuxcnc/src/tool_table.cpp',
  '../../backend/src/linuxcnc/src/loaded_tools.cpp',
  '../../backend/src/linuxcnc/src/nml_panel_source.cpp',
  '../../backend/src/core/include/coffeecnc/core/controller_port.hpp',
  '../../backend/src/core/include/coffeecnc/core/jog_stop_report.hpp',
], { query: '?raw', import: 'default', eager: true })

/** What stands for a value the backend fills in. */
const sample = '7'

/** Literals that read as words but are not operator text, by file. Each must still occur. */
const notOperatorText: Record<string, string[]> = {
  'nml_port.cpp': [
    // The M0 (MSG,...) comment of the breakpoint copy: program text, read back by the port.
    'CoffeeCNC breakpoint ',
    'M0 (MSG,CoffeeCNC breakpoint 7)\n',
    // stderr log lines.
    'coffeecnc: jog_stop_text cause=7 count=7 own_level_ms_before=7 watchdog_tripped_ms_before=7 contact_ms_before=7 text="7"\n',
    'coffeecnc: fixed setter interrupted: ',
    // Content of the spindle hold recovery marker file.
    'CoffeeCNC interrupted spindle hold\n',
  ],
  'preview_engine.cpp': [
    // The preview interpreter's INI file.
    '[RS274NGC]\nPARAMETER_FILE = preview.var\nINI_VARS = 0\nHAL_PIN_VARS = 0\nDISABLE_FANUC_STYLE_SUB = 1\n',
    // Helper diagnostics, logged only.
    '\n[coffeecnc: the helper left processes behind; they were ended]\n',
  ],
  'tool_table.cpp': [
    // A tool table row comment written to the file.
    'reference 7\n',
  ],
}

/**
 * Literals the backend completes in a way this reader does not follow (a ternary joined to
 * more text): the sentences they become.
 */
const completed: Record<string, string[]> = {
  'The probe did not release at the retract position': [
    'The probe did not release at the retract position (no controller status).',
    'The probe did not release at the retract position (probe touching, latched 7, commanded 7 mm, measured 7 mm from the retract).',
  ],
  'The fixed setter did not release at the retract position': [
    'The fixed setter did not release at the retract position (probe clear, latched 7, commanded 7 mm, measured 7 mm from the retract).',
  ],
}

/**
 * Calls that add to the text they are given, by file: tool_model_store.cpp's failure() and the
 * lock's error() append ": " and the system's error text.
 */
const appending: Record<string, RegExp> = {
  'tool_model_store.cpp': /\b(?:failure|error)\(\s*(?:std::format\(\s*)?$/,
}

/** The source with comments blanked out; string and character literals kept. */
function withoutComments(source: string): string {
  let out = ''
  for (let i = 0; i < source.length;) {
    if (source.startsWith('//', i)) {
      const end = source.indexOf('\n', i)
      const stop = end < 0 ? source.length : end
      out += ' '.repeat(stop - i)
      i = stop
    } else if (source.startsWith('/*', i)) {
      const stop = source.indexOf('*/', i) + 2
      out += source.slice(i, stop).replace(/[^\n]/g, ' ')
      i = stop
    } else if (source[i] === '"' || source[i] === "'") {
      const quote = source[i] ?? ''
      let stop = i + 1
      while (stop < source.length && source[stop] !== quote && source[stop] !== '\n') stop += source[stop] === '\\' ? 2 : 1
      out += source.slice(i, stop + 1)
      i = stop + 1
    } else {
      out += source[i] ?? ''
      i += 1
    }
  }
  return out
}

interface Group { start: number; end: number; text: string }

function unescape(body: string): string {
  return body.replace(/\\(.)/g, (_, c: string) => (c === 'n' ? '\n' : c === 't' ? '\t' : c))
}

/** Adjacent string literals joined, as C++ joins them. */
function literalGroups(source: string): Group[] {
  const groups: Group[] = []
  for (const match of source.matchAll(/"((?:[^"\\\n]|\\.)*)"/g)) {
    const start = match.index
    const end = start + match[0].length
    const text = unescape(match[1] ?? '')
    const last = groups.at(-1)
    if (last && source.slice(last.end, start).trim() === '') { last.end = end; last.text += text }
    else groups.push({ start, end, text })
  }
  return groups
}

/** The arguments of a call whose first argument ends at `from`, split at top-level commas. */
function argumentsAfter(source: string, from: number): string[] {
  const args: string[] = []
  let depth = 0
  let current = ''
  for (let i = from; i < source.length; i += 1) {
    const c = source[i] ?? ''
    if (c === '"') {
      const match = /^"(?:[^"\\\n]|\\.)*"/.exec(source.slice(i))
      current += match?.[0] ?? c
      i += (match?.[0].length ?? 1) - 1
      continue
    }
    if ('([{'.includes(c)) depth += 1
    if (')]}'.includes(c)) {
      if (depth === 0) { args.push(current.trim()); break }
      depth -= 1
    }
    if (c === ',' && depth === 0) { args.push(current.trim()); current = ''; continue }
    current += c
  }
  return args.slice(1)
}

/** The operator sentences a source builds, with samples in place of the values. */
function sentences(file: string, raw: string): string[] {
  const source = withoutComments(raw)
  const groups = literalGroups(source)
  const constants = new Map<string, string>()
  for (const group of groups) {
    const name = /constexpr[^;=]*?\b(\w+)\s*=\s*$/.exec(source.slice(Math.max(0, group.start - 120), group.start))?.[1]
    if (name !== undefined) constants.set(name, group.text)
  }
  const operand = (text: string): string => constants.get(text.trim()) ?? sample
  const found: string[] = []
  for (let i = 0; i < groups.length; i += 1) {
    const group = groups[i]
    if (group === undefined) continue
    const before = source.slice(Math.max(0, group.start - 200), group.start)
    let text = group.text
    if (/format\(\s*$/.test(before)) {
      const args = argumentsAfter(source, group.end)
      let index = 0
      text = text.replace(/\{\{|\}\}|\{[^{}]*\}/g, (part) => {
        if (part === '{{') return '{'
        if (part === '}}') return '}'
        const arg = args[index] ?? ''
        index += 1
        return operand(arg)
      })
    }
    const prefix = /(\w*)\s*(?<!\+)\+\s*$/.exec(before)
    if (prefix) text = operand(prefix[1] ?? '') + text
    // `"a" + x + "b"` and `"a" + "b"`: one sentence.
    let last = group
    for (let next = groups[i + 1]; next !== undefined; next = groups[i + 1]) {
      const gap = source.slice(last.end, next.start)
      const middle = /^\s*\+\s*((?:(?!\s:\s)[^;"?])*?)\s*\+\s*$/.exec(gap)
      if (middle) text += operand(middle[1] ?? '') + next.text
      else if (/^\s*\+\s*$/.test(gap)) text += next.text
      else break
      last = next
      i += 1
    }
    const suffix = /^\s*\+(?!\+)\s*(\w*)/.exec(source.slice(last.end))
    if (suffix) text += operand(suffix[1] ?? '')
    if (appending[file]?.test(before)) text += `: ${sample}`
    if (/[a-z]{3,}/.test(text) && text.trim().includes(' ')) found.push(text)
  }
  return found
}

type Flat = Map<string, string>
function flatten(prefix: string, value: unknown, into: Flat): Flat {
  if (typeof value === 'string') into.set(prefix, value)
  else if (typeof value === 'object' && value !== null)
    for (const [key, child] of Object.entries(value)) flatten(prefix === '' ? key : `${prefix}.${key}`, child, into)
  return into
}
const english: Flat = new Map()
for (const text of [messagesCatalog, backendCatalog]) flatten('', JSON.parse(text) as unknown, english)

/** A match that says something: not a pattern made of placeholders alone. */
function known(text: string): boolean {
  const match = matchMessage(text)
  if (match === null) return false
  const pattern = english.get(match.key) ?? ''
  return /[a-z]{3,}/i.test(pattern.replace(/\{\w+\}/g, ''))
}

describe('backend sentences', () => {
  const files = Object.entries(sources).map(([path, raw]) => [path.split('/').at(-1) ?? path, raw] as const)

  it('reads every listed source', () => {
    expect(files.length).toBe(22)
  })

  for (const [file, raw] of files) {
    it(`${file}: every operator sentence is catalogued`, () => {
      const found = sentences(file, raw)
      const skipped = notOperatorText[file] ?? []
      const unknown: string[] = []
      for (const text of found) {
        if (skipped.includes(text)) continue
        for (const sentence of completed[text] ?? [text])
          if (!known(sentence)) unknown.push(sentence)
      }
      expect(unknown).toEqual([])
      // An entry for a literal that is gone is removed with it.
      expect(skipped.filter((text) => !found.includes(text))).toEqual([])
    })
  }
})
