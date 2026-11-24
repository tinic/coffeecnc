// Text that arrives in English from the native backend (command refusals and failures, settings
// and storage reasons, operator messages) and from the panel's own store and browser simulator.
// The protocol carries English fallback text and can also give a message a key and
// parameters. The panel translates at the place the message is shown:
// `parts/messages/en.json` (panel store, simulator, command refusals) and `parts/backend/en.json`
// (native storage, files, preview, LinuxCNC adapter) hold each known sentence, with `{name}`
// for its variable parts, and the matching key gives the translation. Text that matches no sentence, for example LinuxCNC's
// own operator errors, is shown as it arrived. A placeholder is translated in turn only when
// its name is in `nested`: those carry a sentence or a phrase of the catalogs (a reason, an
// action, "longer", a motor count, the commands an action sent, "the probe", "touching", a
// tool model object such as "The file"). A placeholder in `lists` carries such phrases joined
// by ", " (nml_port.cpp, withdraw). Every other placeholder (a path, a file or program name, a
// tool, a G-code line, a number such as the latched probe value) is data and stays exactly as
// it arrived, also when it is a catalog word such as "two".
import type { CommandResult } from '../protocol'
import { i18n, type Params } from './index'
import backend from './parts/backend/en.json?raw'
import panel from './parts/messages/en.json?raw'

const nested = new Set(['detail', 'error', 'reason', 'what', 'where', 'direction', 'motors', 'unsent', 'device', 'input', 'object'])
const lists = new Set(['sent'])

interface Pattern { key: string; regex: RegExp; names: string[] }

const exact = new Map<string, string>()
const patterns: Pattern[] = []
const argumentsByKey = new Map<string, string[]>()

function escape(text: string): string {
  return text.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')
}

function collect(prefix: string, value: unknown): void {
  if (typeof value === 'string') {
    const names: string[] = []
    argumentsByKey.set(prefix, names)
    // vue-i18n literals such as {'@'} stand for their character.
    const english = value.replace(/\{'([^']*)'\}/g, '$1')
    if (!/\{\w+\}/.test(english)) {
      exact.set(english, prefix)
      return
    }
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
      params[name] = translateArgument(name, value)
    })
    return { key: pattern.key, params }
  }
  return null
}

function translateArgument(name: string, value: string): string {
  if (lists.has(name)) return value.split(', ').map((item) => translateMessage(item)).join(i18n.global.t('common.listSeparator'))
  return nested.has(name) ? translateMessage(value) : value
}

function translateList(value: unknown): string | undefined {
  if (!Array.isArray(value) || value.length === 0) return
  const items: string[] = []
  for (const entry of value as unknown[]) {
    if (entry === null || typeof entry !== 'object' || !('text' in entry) || typeof entry.text !== 'string')
      return
    const localization = 'localization' in entry ? entry.localization : undefined
    items.push(translateParameter(localization) ?? entry.text)
  }
  return items.join(i18n.global.t('common.detailListSeparator'))
}

function translateParameter(parameter: unknown): string | undefined {
  if (parameter === null || typeof parameter !== 'object' || !('key' in parameter) ||
      typeof parameter.key !== 'string' || !('params' in parameter)) return
  // Keys for individual arguments carry no nested arguments of their own.
  return translateKey({ key: parameter.key, params: parameter.params })
}

function translateKey(message: unknown): string | undefined {
  if (message === null || typeof message !== 'object' ||
    !('key' in message) || typeof message.key !== 'string' || !('params' in message)) return
  const names = argumentsByKey.get(message.key)
  const args: unknown = message.params
  if (!names || typeof args !== 'object' || args === null || Array.isArray(args)) return
  const localization: unknown = 'params_localization' in message ? message.params_localization : undefined
  const keys = typeof localization === 'object' && localization !== null && !Array.isArray(localization)
    ? localization as Record<string, { key: string; params: Record<string, string> } | undefined>
    : undefined
  const listMetadata = 'params_lists' in message ? message.params_lists : undefined
  const argumentLists = typeof listMetadata === 'object' && listMetadata !== null && !Array.isArray(listMetadata)
    ? listMetadata as Record<string, unknown> : undefined
  const params: Params = {}
  for (const name of names) {
    const value = (args as Record<string, unknown>)[name]
    if (!Object.hasOwn(args, name) || typeof value !== 'string') return
    const parameter = keys && Object.hasOwn(keys, name) ? keys[name] : undefined
    const entries = argumentLists && Object.hasOwn(argumentLists, name) ? argumentLists[name] : undefined
    params[name] = translateList(entries) ?? translateParameter(parameter) ?? translateArgument(name, value)
  }
  return i18n.global.t(message.key as never, params)
}

/** `text` in the panel's language when it is a known sentence; otherwise unchanged. */
export function translateMessage(text: string, localization?: unknown): string {
  const structured = translateKey(localization)
  if (structured !== undefined) return structured
  if (i18n.global.locale.value === 'en' || text === '') return text
  const match = matchMessage(text)
  // The key comes from the English catalog, so it exists in every language.
  return match === null ? text : i18n.global.t(match.key as never, match.params)
}

export function translateResult(result: CommandResult): string {
  return translateMessage(result.reason, result.outcome === 'rejected'
    ? result.refusal ?? result.localization : result.localization)
}
