// Every refusal the browser simulator gives as a plain literal is a catalogued sentence, so the
// panel shows it translated, as it does a native one.
import { describe, expect, it } from 'vitest'

import { matchMessage } from '../src/i18n/messages'

const sources = import.meta.glob<string>('../src/sim/*.ts', { query: '?raw', import: 'default', eager: true })

describe('simulator refusals', () => {
  it('are catalogued sentences', () => {
    const sentences = Object.values(sources).flatMap((source) =>
      [...source.matchAll(/\b(?:reject|fail)\('([^'$]+)'\)/g)].map((match) => match[1] ?? ''))
    expect(sentences.length).toBeGreaterThan(20)
    expect(sentences.filter((sentence) => matchMessage(sentence) === null)).toEqual([])
  })
})
