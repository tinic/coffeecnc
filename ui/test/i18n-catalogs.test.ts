// The message catalogs agree: every language has exactly the English keys, with the same
// placeholders and plural forms, in valid vue-i18n syntax; every key the sources name exists;
// every key exists for a reason (a source names it, or it translates a native sentence).
import { baseCompile } from '@intlify/message-compiler'
import { describe, expect, it } from 'vitest'

import { languages } from '../src/i18n'
import glossary from '../src/i18n/glossary.json'

const raw = import.meta.glob<string>('../src/i18n/parts/*/*.json', { query: '?raw', import: 'default', eager: true })
const sources = import.meta.glob<string>(['../src/**/*.ts', '../src/**/*.vue', '!../src/i18n/parts/**'], { query: '?raw', import: 'default', eager: true })

/** Areas whose keys translate English text that arrives at run time (see src/i18n/messages.ts). */
const sentenceAreas = new Set(['messages', 'backend'])

type Flat = Map<string, string>
function flatten(prefix: string, value: unknown, into: Flat): Flat {
  if (typeof value === 'string') into.set(prefix, value)
  else if (typeof value === 'object' && value !== null)
    for (const [key, child] of Object.entries(value)) flatten(prefix === '' ? key : `${prefix}.${key}`, child, into)
  else into.set(prefix, `<${typeof value}>`)
  return into
}

const catalogs = new Map<string, Map<string, Flat>>() // area -> language -> keys
for (const [path, text] of Object.entries(raw)) {
  const match = /parts\/(\w+)\/(\w+)\.json$/.exec(path)
  if (!match) continue
  const [, area = '', language = ''] = match
  const parsed = JSON.parse(text) as Record<string, unknown>
  const byLanguage = catalogs.get(area) ?? new Map<string, Flat>()
  catalogs.set(area, byLanguage)
  byLanguage.set(language, flatten('', parsed, new Map()))
  // Each area file holds only its own area.
  expect(Object.keys(parsed), path).toEqual([area])
}
const english = new Map<string, string>()
for (const byLanguage of catalogs.values()) for (const [key, value] of byLanguage.get('en') ?? []) english.set(key, value)

const placeholders = (text: string): string[] =>
  [...text.matchAll(/\{(\w+)\}/g)].map((m) => m[1] ?? '').filter((name) => name !== 'count' && name !== 'n').sort()
const forms = (text: string): number => text.split(/(?<!\{')\|(?!'\})/).length

describe('message catalogs', () => {
  it('have every area in every language', () => {
    for (const [area, byLanguage] of catalogs) expect([...byLanguage.keys()].sort(), area).toEqual([...languages].sort())
  })

  for (const language of languages.filter((l) => l !== 'en')) {
    it(`${language} translates exactly the English keys, with the same placeholders`, () => {
      const problems: string[] = []
      for (const [area, byLanguage] of catalogs) {
        const source = byLanguage.get('en') ?? new Map<string, string>()
        const target = byLanguage.get(language) ?? new Map<string, string>()
        for (const key of source.keys()) if (!target.has(key)) problems.push(`missing ${key}`)
        for (const key of target.keys()) if (!source.has(key)) problems.push(`not in English ${key} (${area})`)
        for (const [key, text] of target) {
          const en = source.get(key)
          if (en === undefined) continue
          if (text.trim() === '') problems.push(`empty ${key}`)
          if (placeholders(text).join() !== placeholders(en).join()) problems.push(`placeholders ${key}: ${text}`)
          const plural = forms(en)
          if (plural > 1 && forms(text) !== plural && !(['zh', 'ja'].includes(language) && forms(text) === 1))
            problems.push(`plural forms ${key}: ${text}`)
          if (plural === 1 && forms(text) > 1) problems.push(`stray | in ${key}: ${text}`)
        }
      }
      expect(problems).toEqual([])
    })
  }

  it('are valid vue-i18n messages', () => {
    const problems: string[] = []
    for (const byLanguage of catalogs.values())
      for (const [language, flat] of byLanguage)
        for (const [key, text] of flat) {
          if (text.startsWith('<')) { problems.push(`${language} ${key} is not text`); continue }
          baseCompile(text, { onError: (error) => { problems.push(`${language} ${key}: ${error.message}`) } })
        }
    expect(problems).toEqual([])
  })

  it('name only keys that exist, and every key is used', () => {
    const used = new Set<string>()
    const unknown: string[] = []
    const areas = [...catalogs.keys()]
    for (const [path, text] of Object.entries(sources)) {
      for (const match of text.matchAll(/['`]((?:[a-z]\w*)(?:\.\w+)+)['`]/g)) {
        const key = match[1] ?? ''
        if (english.has(key)) used.add(key)
        else if (areas.includes(key.split('.')[0] ?? '') && !/\.(?:ts|vue|json|js|css)$/.test(key)) unknown.push(`${path}: ${key}`)
      }
    }
    expect(unknown).toEqual([])
    const unused = [...english.keys()].filter((key) => !used.has(key) && !sentenceAreas.has(key.split('.')[0] ?? ''))
    expect(unused).toEqual([])
  })

  it('come with a glossary in every language', () => {
    for (const [term, entry] of Object.entries(glossary.terms))
      expect(Object.keys(entry).sort(), term).toEqual(languages.filter((l) => l !== 'en').sort())
  })
})
