// WCAG contrast of the theme tokens in src/style.css, per theme. theme-contrast.test.ts asserts the
// targets; markdownTable() prints the same table for a review.

export const themes = ['light', 'dark', 'harbor', 'sandstone'] as const
export type Theme = (typeof themes)[number]
type Tokens = Record<string, string>

function block(css: string, selector: string): Tokens {
  const start = css.indexOf(selector)
  if (start < 0) throw new Error(`no theme block ${selector}`)
  const body = css.slice(css.indexOf('{', start) + 1, css.indexOf('}', start))
  return Object.fromEntries([...body.matchAll(/--([\w-]+):\s*([^;]+);/g)].map((m) => [m[1] ?? '', (m[2] ?? '').trim()]))
}

/** The token values of each theme, with the cascade of the theme selectors applied. */
export function themeTokens(css: string): Record<Theme, Tokens> {
  const base = block(css, ':root, .theme-sample {')
  const dark = block(css, ":is(:root, .theme-sample):is([data-theme='dark'], [data-theme='harbor'])")
  const sandstone = block(css, ":is(:root, .theme-sample)[data-theme='sandstone']")
  const harbor = block(css, ":is(:root, .theme-sample)[data-theme='harbor']")
  return { light: base, dark: { ...base, ...dark }, harbor: { ...base, ...dark, ...harbor }, sandstone: { ...base, ...sandstone } }
}

function luminance(hex: string): number {
  const short = /^#([0-9a-f]{3})$/i.exec(hex)?.[1]
  const value = short ? short.replace(/./g, '$&$&') : /^#([0-9a-f]{6})$/i.exec(hex)?.[1]
  if (value === undefined) throw new Error(`not a hex colour: ${hex}`)
  const channel = (i: number): number => {
    const c = parseInt(value.slice(i, i + 2), 16) / 255
    return c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4
  }
  return 0.2126 * channel(0) + 0.7152 * channel(2) + 0.0722 * channel(4)
}

export function contrast(a: string, b: string): number {
  const [x, y] = [luminance(a), luminance(b)]
  return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05)
}

/** [label, foreground token, background token, minimum ratio]. A literal colour starts with '#'. */
export const pairs: readonly (readonly [string, string, string, number])[] = [
  ['Body text on card', 'text', 'panel', 7],
  ['Body text on canvas', 'text', 'bg', 7],
  ['Secondary text on card', 'text-dim', 'panel', 4.5],
  ['Secondary text on canvas', 'text-dim', 'bg', 4.5],
  ['Control label on control', 'text', 'control', 4.5],
  ['Secondary label on control', 'text-dim', 'control', 4.5],
  ['Disabled label on card', 'text-faint', 'panel', 3],
  ['Disabled label on canvas', 'text-faint', 'bg', 3],
  ['Control fill vs card', 'control', 'panel', 1.3],
  ['Control fill vs canvas', 'control', 'bg', 1.3],
  ['Control fill vs sunken track', 'control', 'bg-sunken', 1.2],
  ['Field fill vs card', 'field', 'panel', 1.3],
  ['Field text on field', 'text', 'field', 7],
  ['Card vs canvas', 'panel', 'bg', 1.08],
  ['Selected text on selected fill', 'accent', 'accent-soft', 4.5],
  ['Selected fill vs card', 'accent-soft', 'panel', 1.3],
  ['Selected fill vs canvas', 'accent-soft', 'bg', 1.15],
  ['Primary label on accent', 'on-accent', 'accent', 4.5],
  ['Go label on go fill', 'on-go', 'go-action', 7],
  ['Go fill vs canvas', 'go-action', 'bg', 3],
  ['Danger text on danger fill', 'stop', 'stop-soft', 4.5],
  ['Warning text on warning fill', 'warn', 'warn-soft', 4.5],
  ['Warning fill vs card', 'warn-soft', 'panel', 1.3],
  ['Danger fill vs card', 'stop-soft', 'panel', 1.3],
  ['Go text on go tint', 'go', 'go-soft', 4.5],
  ['Go tint vs card', 'go-soft', 'panel', 1.3],
  ['Edit bar vs code', 'bar', 'bg-sunken', 1.3],
  ['Edit bar button vs edit bar', 'bar-control', 'bar', 1.3],
  ['Label on edit bar', 'text', 'bar', 7],
]

export interface Row { label: string; minimum: number; ratios: Record<Theme, number | null> }

export function contrastTable(css: string): Row[] {
  const tokens = themeTokens(css)
  const colour = (theme: Theme, name: string): string | null => (name.startsWith('#') ? name : tokens[theme][name] ?? null)
  return pairs.map(([label, fg, bg, minimum]) => ({
    label, minimum,
    ratios: Object.fromEntries(themes.map((theme) => {
      const a = colour(theme, fg), b = colour(theme, bg)
      return [theme, a && b ? contrast(a, b) : null]
    })) as Record<Theme, number | null>,
  }))
}

export function markdownTable(css: string): string {
  const lines = [`| Pair | Target | ${themes.join(' | ')} |`, `|---|---|${themes.map(() => '---').join('|')}|`]
  for (const row of contrastTable(css)) {
    const cells = themes.map((theme) => {
      const ratio = row.ratios[theme]
      return ratio === null ? 'n/a' : `${ratio.toFixed(2)}${ratio < row.minimum ? ' ✗' : ''}`
    })
    lines.push(`| ${row.label} | ${String(row.minimum)} | ${cells.join(' | ')} |`)
  }
  return lines.join('\n')
}
