// WebKitGTK shows an element's title attribute as a native GTK tooltip, and leaves it on the
// screen when the element goes away under a touch (turo saw "Starting the restart" stuck over the
// preview). Plain elements use v-tooltip, which hides with its element; only components (dialog
// titles) take a title.
import { describe, expect, it } from 'vitest'

const sources = import.meta.glob<string>('../src/components/*.vue', { query: '?raw', import: 'default', eager: true })

describe('tooltips', () => {
  it('no plain element in a template has a title attribute', () => {
    const offenders: string[] = []
    for (const [file, source] of Object.entries(sources)) {
      const template = source.split('<template>').slice(1).join('<template>')
      for (const tag of template.matchAll(/<([a-z][\w-]*)\b[^>]*?\s:?title=/gs)) offenders.push(`${file}: <${tag[1] ?? ''}>`)
    }
    expect(Object.keys(sources).length).toBeGreaterThan(20)
    expect(offenders).toEqual([])
  })
})
