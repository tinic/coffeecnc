// Display conversion and formatting. The protocol is millimetres throughout; inches exist only
// here.

import type { FeedMode, LengthUnit } from './protocol'

export const mmPerInch = 25.4

export const wcsNames: readonly string[] = ['G54', 'G55', 'G56', 'G57', 'G58', 'G59', 'G59.1', 'G59.2', 'G59.3']

/** Name of work coordinate system 1 … 9. */
export function wcsName(wcs: number): string {
  return wcsNames[wcs - 1] ?? `WCS ${String(wcs)}`
}

export function toDisplay(mm: number, unit: LengthUnit): number {
  return unit === 'inch' ? mm / mmPerInch : mm
}

export function fromDisplay(value: number, unit: LengthUnit): number {
  return unit === 'inch' ? value * mmPerInch : value
}

export function lengthDecimals(unit: LengthUnit): number {
  return unit === 'inch' ? 4 : 3
}

function fixed(value: number, decimals: number): string {
  const text = value.toFixed(decimals)
  // No "-0.000": a value that rounds to zero is zero.
  return Number(text) === 0 ? (0).toFixed(decimals) : text
}

/** A length in the display unit with fixed decimals: 3 for mm, 4 for inch. */
export function formatLength(mm: number, unit: LengthUnit): string {
  return fixed(toDisplay(mm, unit), lengthDecimals(unit))
}

/** A jog increment without trailing zeros: "0.1", "0.0004". */
export function formatIncrement(mm: number, unit: LengthUnit): string {
  return String(Number(toDisplay(mm, unit).toFixed(lengthDecimals(unit) + 1)))
}

/** A linear velocity given in mm/min. */
export function formatVelocity(mmPerMin: number, unit: LengthUnit): string {
  return fixed(toDisplay(mmPerMin, unit), unit === 'inch' ? 1 : 0)
}

export function velocityUnit(unit: LengthUnit): string {
  return unit === 'inch' ? 'in/min' : 'mm/min'
}

/** Unit of the F word for a feed mode. */
export function feedUnit(mode: FeedMode, unit: LengthUnit): string {
  switch (mode) {
    case 'per_minute':
      return velocityUnit(unit)
    case 'inverse_time':
      return '1/min'
    case 'per_revolution':
      return unit === 'inch' ? 'in/rev' : 'mm/rev'
  }
}

/** The F word in effect, in the unit `feedUnit` names. */
export function formatProgrammedFeed(value: number, mode: FeedMode, unit: LengthUnit): string {
  switch (mode) {
    case 'per_minute':
      return formatVelocity(value, unit)
    case 'inverse_time':
      return fixed(value, 2)
    case 'per_revolution':
      return fixed(toDisplay(value, unit), unit === 'inch' ? 4 : 3)
  }
}

/** An override factor as a percentage: 1 is "100 %". */
export function formatPercent(factor: number): string {
  return `${String(Math.round(factor * 100))} %`
}

/** A number as an operator types it; a decimal comma is accepted. Null when it is not a number. */
export function parseNumber(text: string): number | null {
  const cleaned = text.trim().replace(',', '.')
  if (!/^[+-]?(?:\d+\.?\d*|\.\d+)$/.test(cleaned)) {
    return null
  }
  return Number(cleaned)
}

/** A length typed in the display unit, as millimetres. */
export function parseLength(text: string, unit: LengthUnit): number | null {
  const value = parseNumber(text)
  return value === null ? null : fromDisplay(value, unit)
}

/** The last component of a path. */
export function baseName(path: string): string {
  return path.slice(path.lastIndexOf('/') + 1)
}

export function formatBytes(bytes: number): string {
  if (bytes < 1024) {
    return `${String(bytes)} B`
  }
  if (bytes < 1024 * 1024) {
    return `${(bytes / 1024).toFixed(1)} kB`
  }
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`
}

/** Time of day, HH:MM:SS, of a Unix time in milliseconds. */
export function formatClock(time: number): string {
  const date = new Date(time)
  const two = (n: number): string => String(n).padStart(2, '0')
  return `${two(date.getHours())}:${two(date.getMinutes())}:${two(date.getSeconds())}`
}
