// Choosing a language changes the text of the whole panel at once, without a reload: every
// catalog sentence a workspace shows in English is shown in the chosen language instead.
import { flushPromises, mount, type VueWrapper } from '@vue/test-utils'
import { afterAll, beforeAll, describe, expect, it, vi } from 'vitest'
import { nextTick } from 'vue'

import App from '../src/App.vue'
import { languages, setLanguage, type Language } from '../src/i18n'
import { panel, start } from '../src/machine'

const raw = import.meta.glob<string>('../src/i18n/parts/*/*.json', { query: '?raw', import: 'default', eager: true })
function flatten(prefix: string, value: unknown, into: Map<string, string>): Map<string, string> {
  if (typeof value === 'string') into.set(prefix, value)
  else if (typeof value === 'object' && value !== null)
    for (const [key, child] of Object.entries(value)) flatten(prefix === '' ? key : `${prefix}.${key}`, child, into)
  return into
}
const catalog = new Map<Language, Map<string, string>>()
for (const [path, text] of Object.entries(raw)) {
  const language = /\/(\w+)\.json$/.exec(path)?.[1] as Language
  catalog.set(language, flatten('', JSON.parse(text), catalog.get(language) ?? new Map<string, string>()))
}
const english = catalog.get('en') ?? new Map<string, string>()
// Texts the components render with t(): labels and sentences without placeholders. The run-time
// sentence areas are translated through translateMessage and checked in i18n-native.test.ts.
const plain = [...english].filter(([key, text]) => !/[{}|]/.test(text) && text.length >= 4 &&
  !key.startsWith('messages.') && !key.startsWith('backend.'))
/** The keys of each English text: one text can have several keys, translated by context. */
const keysOf = new Map<string, string[]>()
for (const [key, text] of plain) keysOf.set(text, [...(keysOf.get(text) ?? []), key])

let wrapper: VueWrapper
// The simulator ticks for the life of the page. Its ticks must end with this file: one after the
// jsdom environment is gone re-reads window in step.ts's program watch and throws (#279).
const intervals: number[] = []
beforeAll(async () => {
  vi.stubGlobal('ResizeObserver', class { observe(): void { /* Fixed size. */ } disconnect(): void { /* Nothing. */ } })
  Element.prototype.scrollTo = (): void => undefined
  const setInterval = window.setInterval.bind(window)
  vi.spyOn(window, 'setInterval').mockImplementation((...args: Parameters<typeof window.setInterval>) => {
    const id = setInterval(...args)
    intervals.push(id)
    return id
  })
  wrapper = mount(App, { attachTo: document.body })
  start()
  await flushPromises()
})
afterAll(async () => {
  wrapper.unmount()
  expect(intervals.length).toBeGreaterThan(0)
  for (const id of intervals) window.clearInterval(id)
  vi.restoreAllMocks()
  await setLanguage('en')
})

/** Every text the page shows or labels on its own: element texts, text nodes, labels, titles. */
function shownTexts(): Set<string> {
  const texts = new Set<string>()
  const walker = document.createTreeWalker(document.body, NodeFilter.SHOW_ELEMENT | NodeFilter.SHOW_TEXT)
  for (let node = walker.nextNode(); node !== null; node = walker.nextNode()) {
    texts.add((node.textContent ?? '').replace(/[ \t\n\r]+/g, ' ').trim())
    if (node instanceof Element)
      for (const name of ['aria-label', 'aria-description', 'title', 'placeholder']) {
        const value = node.getAttribute(name)
        if (value !== null) texts.add(value.trim())
      }
  }
  return texts
}

/** The English catalog texts the workspace shows now. */
function shownEnglish(): string[] {
  const texts = shownTexts()
  return [...keysOf.keys()].filter((text) => texts.has(text))
}

describe('panel language', () => {
  const workspaces = ['manual', 'program', 'tools', 'probe'] as const
  for (const language of languages.filter((l) => l !== 'en')) {
    it.each(workspaces)(`switches %s to ${language} at once and back to English`, async (workspace) => {
      await setLanguage('en')
      panel.tab = workspace
      await flushPromises()
      const shown = shownEnglish()
      expect(shown.length).toBeGreaterThan(5)
      await setLanguage(language)
      await nextTick()
      expect(document.documentElement.lang).toMatch(new RegExp(`^${language}`))
      const texts = shownTexts()
      const translated = catalog.get(language) ?? new Map<string, string>()
      // Each English text gives way to a translation of one of its keys; a word that is the same
      // in both languages (Simulator) may stay.
      const untranslated = shown.filter((text) => {
        const values = (keysOf.get(text) ?? []).map((key) => translated.get(key) ?? '')
        if (values.includes(text)) return false
        return texts.has(text) || !values.some((value) => texts.has(value))
      })
      expect(untranslated).toEqual([])
      await setLanguage('en')
      await nextTick()
      expect(shownEnglish()).toEqual(shown)
    })
  }
})
