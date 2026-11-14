import { describe, expect, it } from 'vitest'

import {
  baseName,
  feedUnit,
  formatBytes,
  formatIncrement,
  formatLength,
  formatPercent,
  formatProgrammedFeed,
  formatVelocity,
  fromDisplay,
  parseLength,
  parseNumber,
  toDisplay,
  velocityUnit,
  wcsName,
} from '../src/units'

describe('unit conversion', () => {
  it('converts millimetres for display and back', () => {
    expect(toDisplay(25.4, 'inch')).toBeCloseTo(1, 12)
    expect(toDisplay(25.4, 'mm')).toBe(25.4)
    expect(fromDisplay(2, 'inch')).toBeCloseTo(50.8, 12)
    expect(fromDisplay(2, 'mm')).toBe(2)
    expect(fromDisplay(toDisplay(123.456, 'inch'), 'inch')).toBeCloseTo(123.456, 9)
  })
})

describe('formatting', () => {
  it('shows lengths with three decimals in mm and four in inch', () => {
    expect(formatLength(12.3456, 'mm')).toBe('12.346')
    expect(formatLength(-0.5, 'mm')).toBe('-0.500')
    expect(formatLength(25.4, 'inch')).toBe('1.0000')
    expect(formatLength(-150, 'inch')).toBe('-5.9055')
  })

  it('never shows a negative zero', () => {
    expect(formatLength(-0.0001, 'mm')).toBe('0.000')
    expect(formatLength(-0.001, 'inch')).toBe('0.0000')
    expect(formatLength(0, 'mm')).toBe('0.000')
  })

  it('shows jog increments without trailing zeros', () => {
    expect(formatIncrement(0.01, 'mm')).toBe('0.01')
    expect(formatIncrement(10, 'mm')).toBe('10')
    expect(formatIncrement(0.0254, 'inch')).toBe('0.001')
    expect(formatIncrement(1, 'inch')).toBe('0.03937')
  })

  it('shows velocities per minute', () => {
    expect(formatVelocity(599.6, 'mm')).toBe('600')
    expect(formatVelocity(254, 'inch')).toBe('10.0')
    expect(velocityUnit('mm')).toBe('mm/min')
    expect(velocityUnit('inch')).toBe('in/min')
  })

  it('labels the programmed feed by its feed mode', () => {
    expect(feedUnit('per_minute', 'mm')).toBe('mm/min')
    expect(feedUnit('per_minute', 'inch')).toBe('in/min')
    expect(feedUnit('inverse_time', 'mm')).toBe('1/min')
    expect(feedUnit('per_revolution', 'mm')).toBe('mm/rev')
    expect(feedUnit('per_revolution', 'inch')).toBe('in/rev')
    expect(formatProgrammedFeed(600, 'per_minute', 'mm')).toBe('600')
    expect(formatProgrammedFeed(2.5, 'inverse_time', 'inch')).toBe('2.50')
    expect(formatProgrammedFeed(0.127, 'per_revolution', 'mm')).toBe('0.127')
    expect(formatProgrammedFeed(0.127, 'per_revolution', 'inch')).toBe('0.0050')
  })

  it('shows overrides as percentages', () => {
    expect(formatPercent(1)).toBe('100 %')
    expect(formatPercent(0.255)).toBe('26 %')
    expect(formatPercent(0)).toBe('0 %')
  })

  it('names the work coordinate systems', () => {
    expect([1, 6, 7, 9].map(wcsName)).toEqual(['G54', 'G59', 'G59.1', 'G59.3'])
    expect(wcsName(12)).toBe('WCS 12')
  })

  it('shows file names and sizes', () => {
    expect(baseName('/home/sim/nc_files/slot.ngc')).toBe('slot.ngc')
    expect(baseName('slot.ngc')).toBe('slot.ngc')
    expect(formatBytes(157)).toBe('157 B')
    expect(formatBytes(2048)).toBe('2.0 kB')
    expect(formatBytes(3 * 1024 * 1024)).toBe('3.0 MB')
  })
})

describe('number entry', () => {
  it('accepts what an operator types', () => {
    expect(parseNumber('12.5')).toBe(12.5)
    expect(parseNumber(' -0,25 ')).toBe(-0.25)
    expect(parseNumber('.5')).toBe(0.5)
    expect(parseNumber('+3')).toBe(3)
    expect(parseNumber('7.')).toBe(7)
  })

  it('refuses what is not a number', () => {
    for (const text of ['', ' ', '-', '.', '1.2.3', '12a', '1e3', '--1', 'NaN', 'Infinity']) {
      expect(parseNumber(text)).toBeNull()
    }
  })

  it('reads lengths in the display unit', () => {
    expect(parseLength('1', 'inch')).toBeCloseTo(25.4, 12)
    expect(parseLength('1', 'mm')).toBe(1)
    expect(parseLength('x', 'mm')).toBeNull()
  })
})
