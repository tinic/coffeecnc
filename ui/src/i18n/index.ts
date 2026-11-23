// Operator-panel text in seven languages, with vue-i18n. English is bundled and is the fallback;
// each other language is its own chunk, loaded when it is chosen. Components use `useI18n()`;
// plain modules use `i18n.global.t`. Changing `locale` updates every rendered text at once.
//
// Numbers: machine values (coordinates, offsets, feeds, speeds, tool data, G-code) keep their
// technical form in every language: '.' as the decimal separator, no grouping, units such as mm,
// in, mm/min and rpm, axis letters and G/M words unchanged. Dates and clock times follow the
// language. Plural forms are "one | other" in the European languages and one form in Chinese
// and Japanese; Intl.PluralRules picks the form.
import { createI18n } from 'vue-i18n'

import en from './parts/en'

export type MessageSchema = typeof en
export type Params = Record<string, string | number>

export const languages = ['en', 'fr', 'de', 'es', 'pt', 'zh', 'ja'] as const
export type Language = typeof languages[number]

/** Each language in its own name, as the picker shows it. */
export const languageNames: Record<Language, string> = {
  en: 'English', fr: 'Français', de: 'Deutsch', es: 'Español', pt: 'Português',
  zh: '简体中文', ja: '日本語',
}

/** BCP 47 tags: European Portuguese, Simplified Chinese. */
export const languageTags: Record<Language, string> = {
  en: 'en', fr: 'fr', de: 'de', es: 'es', pt: 'pt-PT', zh: 'zh-Hans', ja: 'ja',
}

export function decodeLanguage(value: unknown): Language | null {
  return languages.find((language) => language === value) ?? null
}

const loaders: Record<Exclude<Language, 'en'>, () => Promise<{ default: MessageSchema }>> = {
  fr: () => import('./parts/fr'),
  de: () => import('./parts/de'),
  es: () => import('./parts/es'),
  pt: () => import('./parts/pt'),
  zh: () => import('./parts/zh'),
  ja: () => import('./parts/ja'),
}

/** "one | other" by the language's CLDR rule; a single form is used for every count. */
function pluralRule(language: Language): (choice: number, choices: number) => number {
  const rules = new Intl.PluralRules(languageTags[language])
  return (choice, choices) => choices < 2 ? 0 : rules.select(choice) === 'one' ? 0 : 1
}

const time = { hour: '2-digit', minute: '2-digit', second: '2-digit' } as const
const datetimeFormats = Object.fromEntries(languages.map((language) => [language, {
  time, short: { dateStyle: 'medium', timeStyle: 'short' },
}])) as Record<Language, { time: typeof time; short: Intl.DateTimeFormatOptions }>

export const i18n = createI18n<[MessageSchema], Language, false>({
  // Composition mode; vue-i18n 12 drops the option and the legacy mode with it.
  // eslint-disable-next-line @typescript-eslint/no-deprecated
  legacy: false,
  locale: 'en',
  fallbackLocale: 'en',
  messages: { en } as Record<Language, MessageSchema>,
  pluralRules: Object.fromEntries(languages.map((language) => [language, pluralRule(language)])),
  datetimeFormats,
  // A missing translation falls back to English; tests make a missing key fail.
  fallbackWarn: false,
})

declare module 'vue-i18n' {
  // Typed keys for `useI18n()` and `i18n.global.t`.
  // eslint-disable-next-line @typescript-eslint/no-empty-object-type
  export interface DefineLocaleMessage extends MessageSchema {}
}

/** The panel's language. Change it with `setLanguage`. */
export const language = i18n.global.locale

let requested: Language = 'en'

/** Loads the language's text, then switches the whole panel to it. A later call wins. */
export async function setLanguage(next: Language): Promise<void> {
  requested = next
  if (next !== 'en' && !i18n.global.availableLocales.includes(next)) {
    const messages = (await loaders[next]()).default
    i18n.global.setLocaleMessage(next, messages)
  }
  if (requested !== next) return
  i18n.global.locale.value = next
  if (typeof document !== 'undefined') document.documentElement.lang = languageTags[next]
}

/** For modules outside components: the text of `key` in the panel's language. */
export const t = i18n.global.t

/** A clock time in the panel's language, for example in the log. */
export function formatTime(value: number): string {
  return i18n.global.d(value, 'time')
}
