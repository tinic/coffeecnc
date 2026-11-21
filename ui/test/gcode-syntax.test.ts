import { describe, expect, it } from 'vitest'

import { tokenizeGcodeLine } from '../src/gcode-syntax'

function tones(text: string): [string, string][] {
  const tokens = tokenizeGcodeLine(text)
  expect(tokens.map((token) => token.text).join('')).toBe(text)
  return tokens.flatMap(({ text: word, tone }) => tone === null ? [] : [[word, tone]])
}

describe('G-code syntax', () => {
  it('groups motion, coordinates, rates, tool and spindle words while preserving layout', () => {
    expect(tones('n 12 g00 X -.5 y+2 z.75  g1 F600 S1500 T1 M06 H1 M3 M9')).toEqual([
      ['n 12', 'modal'], ['g00', 'rapid'], ['X -.5', 'x'], ['y+2', 'y'], ['z.75', 'z'],
      ['g1', 'cut'], ['F600', 'rate'], ['S1500', 'rate'], ['T1', 'tool'],
      ['M06', 'tool'], ['H1', 'tool'], ['M3', 'spindle'], ['M9', 'spindle'],
    ])
  })

  it('distinguishes drilling arguments and program stops from setup words', () => {
    expect(tones('G17 G98 G83Z5 R10 Q2 P0.5 M00 M1 M2 M30 M60')).toEqual([
      ['G17', 'modal'], ['G98', 'modal'], ['G83', 'cut'], ['Z5', 'z'], ['R10', 'argument'],
      ['Q2', 'argument'], ['P0.5', 'argument'],
      ['M00', 'stop'], ['M1', 'stop'], ['M2', 'stop'], ['M30', 'stop'], ['M60', 'stop'],
    ])
  })

  it('leaves operator messages and comments intact, including command-like text', () => {
    expect(tones('(g91.1 checked) G2 ( msg, watch X11 ) I-3 ; M30 is a comment')).toEqual([
      ['(g91.1 checked)', 'comment'], ['G2', 'cut'], ['( msg, watch X11 )', 'message'],
      ['I-3', 'argument'], ['; M30 is a comment', 'comment'],
    ])
  })

  it('preserves named parameters, nested expressions, and numeric or named O-words', () => {
    expect(tones('o <pocket> if [#<depth> LT [#1 + 2]]')).toEqual([
      ['o <pocket>', 'flow'], ['if', 'flow'], ['[#<depth> LT [#1 + 2]]', 'expression'],
    ])
    expect(tones('X#<step> Y [#2+2] o123call')).toEqual([
      ['X', 'x'], ['#<step>', 'parameter'], ['Y', 'y'], ['[#2+2]', 'expression'],
      ['o123', 'flow'], ['call', 'flow'],
    ])
  })

  it('keeps unfamiliar and unfinished text visible', () => {
    expect(tones('  % A20 (unfinished M0')).toEqual([['(unfinished M0', 'comment']])
    expect(tones(' \t ')).toEqual([])
    expect(tokenizeGcodeLine('')).toEqual([])
  })
})
