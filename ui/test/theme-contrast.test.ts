import { describe, expect, it } from 'vitest'
import css from '../src/style.css?raw'
import { contrastTable, themes } from './theme-contrast'

describe('theme contrast', () => {
  for (const row of contrastTable(css)) {
    for (const theme of themes) {
      it(`${theme}: ${row.label} is at least ${String(row.minimum)}:1`, () => {
        expect(row.ratios[theme]).not.toBeNull()
        expect(row.ratios[theme]).toBeGreaterThanOrEqual(row.minimum)
      })
    }
  }

  // Separation comes from tone, not outlines (owner direction; see docs/design/implementation-alignment.md).
  it('gives buttons, choices and fields no outline or edge line', () => {
    const rule = (selector: string): string => {
      const start = css.indexOf(`\n${selector} {`)
      expect(start).toBeGreaterThanOrEqual(0)
      return css.slice(start, css.indexOf('}', start))
    }
    for (const selector of ['.btn, .choice', '.btn', '.field']) {
      const body = rule(selector)
      expect(body).not.toMatch(/border:\s*1px solid/)
      expect(body).not.toMatch(/box-shadow/)
    }
    expect(css).not.toContain('--control-edge')
    expect(css).not.toContain('--button-edge')
  })
})
