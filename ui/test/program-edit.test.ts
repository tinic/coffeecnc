import { afterEach, describe, expect, it, vi } from 'vitest'
import { nextTick } from 'vue'
import { mount, type VueWrapper } from '@vue/test-utils'

import NcText from '../src/components/NcText.vue'
import { feedOrSpeedEdit, rateWords, withRate } from '../src/program-edit'
import { rig, testProgram } from './harness'

describe('feed and speed edits', () => {
  it('finds the F and S numbers outside comments', () => {
    expect(rateWords('G1 X10 F600 S1500 (F9) ; S8').map((w) => [w.letter, w.value])).toEqual([['F', 600], ['S', 1500]])
    expect(rateWords('g1 f 12.5')).toMatchObject([{ letter: 'F', value: 12.5 }])
    expect(rateWords('G1 F#1 S[100]')).toEqual([])
  })

  it('accepts exactly what the native writer accepts', () => {
    const accepted: [string, string][] = [
      ['G1 X10 F600', 'G1 X10 F450'], ['S1500 M3', 'S2000 M3'], ['g1 x1 f 300', 'g1 x1 f 250.5'],
      ['G1 X10 F600 S1000', 'G1 X10 F600 S900'], ['F.5', 'F12.'],
    ]
    const refused: [string, string][] = [
      ['G1 X10 F600', 'G1 X10 F600'], ['G1 X10 F600', 'G1 X11 F600'], ['G1 X10 F600', 'G1 X10 F600 G0 Z-5'],
      ['G1 X10 F600', 'G1 X10 F[600]'], ['G1 X10 F600', 'G1 X10 F-600'], ['G1 X10 F600', 'G1 X10 F6e2'],
      ['G1 X10 F600', 'G1 X10 F60 0'], ['G1 X10 F600', 'G1 X10 F6\n0'], ['(F600) G1 X1', '(F500) G1 X1'],
      ['G1 X1 ; F600', 'G1 X1 ; F500'], ['G1 X1 F#1', 'G1 X1 F#2'],
    ]
    for (const [before, after] of accepted) expect(feedOrSpeedEdit(before, after), `${before} → ${after}`).toBe(true)
    for (const [before, after] of refused) expect(feedOrSpeedEdit(before, after), `${before} → ${after}`).toBe(false)
  })

  it('writes the new number plainly', () => {
    const [feed] = rateWords('G1 X10 F600')
    if (feed === undefined) throw new Error('no feed')
    expect(withRate('G1 X10 F600', feed, 450)).toBe('G1 X10 F450')
    expect(withRate('G1 X10 F600', feed, 0.0000001)).toBe('G1 X10 F0')
    expect(withRate('G1 X10 F600', feed, 1e21)).toBeNull()
    expect(withRate('G1 X10 F600', feed, -1)).toBeNull()
  })

  it('saves the program in the simulator, opens it again and previews the new feed', () => {
    const r = rig()
    r.home()
    r.load('G21 G90\nG0 Z5\nG1 X10 F600\nM30\n')
    const revision = r.status().program.revision
    const edited = r.send({ type: 'edit_program', path: testProgram, line: 3, old_text: 'G1 X10 F600', new_text: 'G1 X10 F450' })
    expect(edited.outcome).toBe('accepted')
    r.sim.advance(400)
    expect(r.status().program.revision).toBeGreaterThan(revision)
    expect(r.preview().segments.find((s) => s.line === 3)?.feed).toBe(450)
    expect(r.send({ type: 'edit_program', path: testProgram, line: 3, old_text: 'G1 X10 F600', new_text: 'G1 X10 F300' })).toMatchObject({
      outcome: 'failed', reason: 'The program changed since it was shown. Open it again.',
    })
    expect(r.send({ type: 'edit_program', path: testProgram, line: 3, old_text: 'G1 X10 F450', new_text: 'G1 X20 F450' })).toMatchObject({
      outcome: 'rejected', reason: 'Only the number of one F or S word can be changed.',
    })
  })
})

describe('tapping a feed in the code list', () => {
  let wrapper: VueWrapper | null = null
  afterEach(() => { wrapper?.unmount(); wrapper = null; vi.unstubAllGlobals() })

  it('offers F and S words only while the program is idle', async () => {
    Object.defineProperty(document, 'fonts', { configurable: true, value: { ready: Promise.resolve() } })
    vi.stubGlobal('ResizeObserver', class { observe(): void { /* fixed */ } disconnect(): void { /* none */ } })
    const pane = mount(NcText, { props: { lines: ['G21', 'G1 X10 F600'], motionLine: 0, currentLine: 0, errorLine: 0, restartable: false } })
    wrapper = pane
    await nextTick()
    expect(pane.find('.editable').exists()).toBe(false)
    await pane.setProps({ restartable: true })
    const feed = pane.get('.editable')
    expect(feed.text()).toContain('600')
    await feed.trigger('click')
    const [line, offset] = pane.emitted<[number, number]>('editRate')?.[0] ?? [0, 0]
    expect(line).toBe(2)
    expect(rateWords('G1 X10 F600').find((word) => word.end > offset)?.value).toBe(600)
  })
})
