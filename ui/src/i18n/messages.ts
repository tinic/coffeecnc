// Text that arrives in English from the native backend (command refusals and failures, settings
// and storage reasons, operator messages) and from the panel's own store and browser simulator.
// The protocol carries it as English sentences, so the panel translates it where it is shown:
// `parts/messages/en.json` (panel store, simulator, command refusals) and `parts/backend/en.json`
// (native storage, files, preview, LinuxCNC adapter) hold each known sentence, with `{name}`
// for its variable parts, and the matching key gives the translation. Text that matches no sentence, for example LinuxCNC's
// own operator errors, is shown as it arrived. A placeholder is translated in turn only when
// its name is in `nested`: those carry a sentence or a phrase of the catalogs (a reason, an
// action, "longer", a motor count). Every other placeholder (a path, a file or program name, a
// tool, a G-code line, a number) is data and stays exactly as it arrived, also when it is a
// catalog word such as "two".
import { i18n, type Params } from './index'
import backend from './parts/backend/en.json?raw'
import panel from './parts/messages/en.json?raw'

const nested = new Set(['detail', 'error', 'reason', 'what', 'where', 'direction', 'motors'])

interface Pattern { key: string; regex: RegExp; names: string[] }

const exact = new Map<string, string>()
const patterns: Pattern[] = []

function escape(text: string): string {
  return text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
}

function collect(prefix: string, value: unknown): void {
  if (typeof value === 'string') {
    const names: string[] = []
    // vue-i18n literals such as {'@'} stand for their character.
    const english = value.replace(/\{'([^']*)'\}/g, '$1')
    if (!/\{\w+\}/.test(english)) { exact.set(english, prefix); return }
    const regex = english.split(/(\{\w+\})/).map((part) => {
      const name = /^\{(\w+)\}$/.exec(part)?.[1]
      if (name === undefined) return escape(part)
      names.push(name)
      return '(.+?)'
    }).join('')
    patterns.push({ key: prefix, regex: new RegExp(`^${regex}$`, 's'), names })
    return
  }
  if (typeof value === 'object' && value !== null)
    for (const [key, child] of Object.entries(value)) collect(prefix === '' ? key : `${prefix}.${key}`, child)
}
collect('', JSON.parse(panel))
collect('', JSON.parse(backend))
// Longer, more specific sentences first.
patterns.sort((a, b) => b.regex.source.length - a.regex.source.length)

/** The message key and parameters of an English sentence, or null for unknown text. */
export function matchMessage(text: string): { key: string; params: Params } | null {
  const key = exact.get(text)
  if (key !== undefined) return { key, params: {} }
  for (const pattern of patterns) {
    const match = pattern.regex.exec(text)
    if (match === null) continue
    const params: Params = {}
    pattern.names.forEach((name, index) => {
      const value = match[index + 1] ?? ''
      params[name] = nested.has(name) ? translateMessage(value) : value
    })
    return { key: pattern.key, params }
  }
  return null
}

/** `text` in the panel's language when it is a known sentence; otherwise unchanged. */
export function translateMessage(text: string): string {
  if (i18n.global.locale.value === 'en' || text === '') return text
  const match = matchMessage(text)
  // The key comes from the English catalog, so it exists in every language.
  return match === null ? text : i18n.global.t(match.key as never, match.params)
}
