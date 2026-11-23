// Small arithmetic grammar for operator entry. Never executes JavaScript or G-code.
// A unit qualifies its multiplicative term: 7/2mm and 7mm/2 both mean 3.5 mm.
// Bare terms use the field's displayed unit; explicit lengths convert into that unit.
interface Quantity { value: number; unit: boolean }

export function parseNumericExpression(text: string, displayedUnit = ''): number | null {
  if (text.length > 256) return null
  const source = text.trim().toLowerCase().replaceAll(',', '.').replaceAll('×', '*').replaceAll('÷', '/')
  const unit = displayedUnit === 'inch' ? 'in' : displayedUnit.toLowerCase()
  let position = 0
  let depth = 0
  const space = (): void => { while (/\s/.test(source[position] ?? '') && position < source.length) position++ }
  const peek = (): string => { space(); return source[position] ?? '' }
  const fail = (): never => { throw new Error('Invalid numeric expression') }
  const finite = (value: number, explicit: boolean): Quantity => {
    if (!Number.isFinite(value)) fail()
    return { value, unit: explicit }
  }
  const qualify = (quantity: Quantity): Quantity => {
    space()
    const suffix = /^(?:inch|mm|in|rpm|deg|°|%|s)/.exec(source.slice(position))?.[0]
    if (suffix === undefined) return quantity
    if (quantity.unit) fail()
    position += suffix.length
    if ((unit === 'mm' || unit === 'in') && ['mm', 'in', 'inch'].includes(suffix)) {
      const factor = suffix === 'mm' ? 1 : 25.4
      return finite(quantity.value * factor / (unit === 'in' ? 25.4 : 1), true)
    }
    if (suffix === unit || (unit === '°' && suffix === 'deg')) return finite(quantity.value, true)
    return fail()
  }
  const primary = (): Quantity => {
    if (++depth > 16) fail()
    let result: Quantity
    if (peek() === '(') {
      position++
      result = sum()
      if (peek() !== ')') fail()
      position++
    } else {
      space()
      const number = /^(?:\d+\.?\d*|\.\d+)/.exec(source.slice(position))?.[0]
      if (number === undefined) return fail()
      position += number.length
      result = finite(Number(number), false)
    }
    depth--
    return result
  }
  const signed = (): Quantity => {
    const sign = peek()
    if (sign === '+' || sign === '-') position++
    const result = primary()
    return finite(sign === '-' ? -result.value : result.value, result.unit)
  }
  const product = (): Quantity => {
    let left = qualify(signed())
    while (peek() === '*' || peek() === '/') {
      const operator = source[position++]
      const right = signed()
      if (operator === '/' && (right.value === 0 || right.unit) || left.unit && right.unit) fail()
      left = qualify(finite(operator === '*' ? left.value * right.value : left.value / right.value, left.unit || right.unit))
    }
    return left
  }
  const sum = (): Quantity => {
    let left = product()
    while (peek() === '+' || peek() === '-') {
      const operator = source[position++]
      const right = product()
      left = finite(operator === '+' ? left.value + right.value : left.value - right.value, left.unit || right.unit)
    }
    return left
  }
  try {
    const result = sum()
    return peek() === '' ? result.value : null
  } catch {
    return null
  }
}
