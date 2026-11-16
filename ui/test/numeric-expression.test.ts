import { describe, expect, it } from 'vitest'
import { parseNumericExpression as parse } from '../src/numeric-expression'
import { parseLength, parseNumber } from '../src/units'

describe('operator formulas', () => {
  it.each([
    ['1/4in', 6.35], ['7mm/2', 3.5], ['7/2mm', 3.5],
    ['1in+7/2mm', 28.9], ['1in+2mm', 27.4], ['(1in+2mm)/2', 13.7],
    ['2*(1in+2mm)', 54.8], ['(7/2)mm', 3.5], ['25.4mm-1in', 0],
    ['1in + 2', 27.4], ['-1/4in', -6.35], [' 1 / 4 IN ', 6.35],
    ['0,5in', 12.7], ['7÷2mm', 3.5],
  ] as const)('converts %s to millimeters', (formula, expected) => {
    expect(parse(formula, 'mm')).toBeCloseTo(expected, 12)
    expect(parseLength(formula, 'mm')).toBeCloseTo(expected, 12)
  })
  it.each(['mm', 'inch'] as const)('converts explicit lengths once in a %s field', (unit) => {
    expect(parseLength('1/4in', unit)).toBeCloseTo(6.35, 12)
    expect(parseLength('7mm/2', unit)).toBeCloseTo(3.5, 12)
    expect(parseLength('7/2mm', unit)).toBeCloseTo(3.5, 12)
  })
  it('uses displayed inches for bare terms', () => {
    expect(parse('7/2', 'inch')).toBe(3.5)
    expect(parseLength('7/2', 'inch')).toBeCloseTo(88.9, 12)
    expect(parse('7mm/2', 'inch')).toBeCloseTo(3.5 / 25.4, 12)
  })
  it('evaluates arithmetic with precedence, parentheses and signed operands', () => {
    expect(parseNumber('1000+600/2')).toBe(1300)
    expect(parseNumber('(1000+600)/2')).toBe(800)
    expect(parseNumber('7/-2')).toBe(-3.5)
    expect(parseNumber('1--2')).toBe(3)
    expect(parseNumber('2×3+4÷2')).toBe(8)
    expect(parse('2400rpm/2', 'rpm')).toBe(1200)
    expect(parse('90deg/2', '°')).toBe(45)
    expect(parse('100%/2', '%')).toBe(50)
  })
  it.each(['', '.', '--1', '1e3', '1/0', '0/0', '1+', '(2+3', '1 2', '2(3)',
    '7/2mm garbage', '1mm*2mm', '1mm/2in', '1/(2mm)', '1cm', 'Math.PI',
    'alert(1)', '1;2', 'Infinity', 'NaN', '2**3', '1<<2', '1/*2*/3',
    '('.repeat(20) + '1' + ')'.repeat(20), '9'.repeat(257)])('rejects %j', (text) => {
    expect(parse(text, 'mm')).toBeNull()
  })
  it('rejects incompatible unit suffixes', () => {
    expect(parseNumber('1in')).toBeNull()
    expect(parse('1/4in', 'rpm')).toBeNull()
    expect(parse('90deg', 'mm')).toBeNull()
    expect(parse('50%', 'mm')).toBeNull()
  })
})
