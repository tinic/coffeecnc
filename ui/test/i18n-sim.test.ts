// Every refusal the browser simulator gives as a plain literal is a catalogued sentence, so the
// panel shows it translated, as it does a native one.
import { describe, expect, it } from 'vitest'
import ts from 'typescript'

import { i18n } from '../src/i18n'
import { matchMessage } from '../src/i18n/messages'

const sources = import.meta.glob<string>('../src/sim/*.ts', { query: '?raw', import: 'default', eager: true })

describe('simulator refusals', () => {
  it('are catalogued sentences', () => {
    const sentences = Object.values(sources).flatMap((source) =>
      [...source.matchAll(/\b(?:reject|fail)\('([^'$]+)'\)/g)].map((match) => match[1] ?? ''))
    expect(sentences.length).toBeGreaterThan(20)
    expect(sentences.filter((sentence) => matchMessage(sentence) === null)).toEqual([])
  })

  it('uses the matching English catalog key for each keyed literal', () => {
    let checked = 0
    for (const [path, source] of Object.entries(sources)) {
      const file = ts.createSourceFile(path, source, ts.ScriptTarget.Latest, true)
      function visit(node: ts.Node): void {
        if (ts.isCallExpression(node) && ts.isIdentifier(node.expression) && node.expression.text === 'reject') {
          const [reason, key] = node.arguments
          if (reason && ts.isStringLiteral(reason) && key) {
            expect(ts.isStringLiteral(key), path).toBe(true)
            if (ts.isStringLiteral(key)) {
              expect(i18n.global.te(key.text, 'en'), path).toBe(true)
              expect(i18n.global.t(key.text as never, {}, { locale: 'en' }), key.text).toBe(reason.text)
              checked += 1
            }
          }
        }
        ts.forEachChild(node, visit)
      }
      visit(file)
    }
    expect(checked).toBeGreaterThan(20)
  })
})
