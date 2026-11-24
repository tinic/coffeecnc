import { describe, expect, it } from 'vitest'
import css from '../src/style.css?raw'
import { contrastTable, themes } from './theme-contrast'

describe('theme contrast', () => {
  for (const row of contrastTable(css)) {
    for (const theme of themes) {
      it(`${theme}: ${row.label} is at least ${String(row.minimum)}:1`, () => {
        expect(row.ratios[theme]).not.toBeNull()
        expect(row.ratios[theme]).toBeGreaterThanOrEqual(row.minimum)
        expect(row.ratios[theme]).toBeLessThanOrEqual(row.maximum)
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
    expect(rule('.btn, button.choice')).toMatch(/appearance:\s*none/)
    expect(rule('.btn, button.choice')).toMatch(/border:\s*0\s*;/)
    for (const selector of ['.btn, .choice', '.btn, button.choice', '.field']) {
      const body = rule(selector)
      expect(body).not.toMatch(/border:\s*1px solid/)
      expect(body).not.toMatch(/box-shadow/)
    }
    expect(css).not.toContain('--control-edge')
    expect(css).not.toContain('--button-edge')
  })

  // A control that is pressed (or selected, or primary) and disabled shows the disabled look: every
  // state rule after the disabled rule must leave disabled controls out.
  it('lets no pressed, selected or coloured state override a disabled control', () => {
    const disabledAt = css.indexOf('\n.btn:disabled,\n.btn[aria-disabled=\'true\'],')
    expect(disabledAt).toBeGreaterThan(0)
    const later = css.slice(disabledAt + 1)
    const rules = [...later.matchAll(/(?:^|\n)([^{}@/]+)\{[^}]*background[^}]*\}/g)].map((m) => m[1] ?? '')
    const selectors = rules.flatMap((rule) => rule.split(/,(?![^()]*\))/).map((s) => s.trim()))
      .filter((s) => /^\.(btn|choice)\b/.test(s) && /\.on\b|aria-pressed|aria-current|\.selected|\.primary|\.go\b|\.warning|\.danger/.test(s))
    expect(selectors.length).toBeGreaterThan(5)
    for (const selector of selectors) expect(selector, selector).toContain(":not(:disabled, [aria-disabled='true'])")
  })
})
