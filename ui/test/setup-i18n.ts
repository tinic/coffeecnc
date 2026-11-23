// Every mounted component gets the panel's vue-i18n instance, in English unless a test chooses
// another language. A key missing from the catalogs fails the test that renders it.
import { config } from '@vue/test-utils'
import { afterEach } from 'vitest'

import { i18n, setLanguage } from '../src/i18n'

config.global.plugins = [...config.global.plugins, i18n]
i18n.global.setMissingHandler((locale, key) => {
  throw new Error(`Missing ${locale} message: ${key}`)
})
afterEach(async () => { await setLanguage('en') })
