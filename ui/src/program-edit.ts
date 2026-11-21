// Editing the number of one F (feed) or S (spindle speed) word of a program line. The native
// program writer applies the same rule (feed_or_speed_edit); nothing else of a line may change.

export interface RateWord {
  letter: 'F' | 'S'
  /** Index of the first digit (or point) of the number in the line, and one past its end. */
  begin: number
  end: number
  value: number
}

const digit = (c: string): boolean => c >= '0' && c <= '9'

/** The F and S words with a plain number, outside `(…)` comments and after-`;` comments. */
export function rateWords(text: string): RateWord[] {
  const words: RateWord[] = []
  let comment = false
  for (let i = 0; i < text.length; i += 1) {
    const c = text.charAt(i)
    if (comment) {
      comment = c !== ')'
      continue
    }
    if (c === '(') {
      comment = true
      continue
    }
    if (c === ';') break
    const letter = c.toUpperCase()
    if (letter !== 'F' && letter !== 'S') continue
    let begin = i + 1
    while (text.charAt(begin) === ' ') begin += 1
    let end = begin
    while (end < text.length && (digit(text.charAt(end)) || text.charAt(end) === '.')) end += 1
    const number = text.slice(begin, end)
    if (plainDecimal(number)) words.push({ letter, begin, end, value: Number(number) })
  }
  return words
}

export function plainDecimal(text: string): boolean {
  return /^(\d+\.?\d*|\.\d+)$/.test(text)
}

/** The line with `word`'s number replaced by `value`, written without an exponent. */
export function withRate(text: string, word: RateWord, value: number): string | null {
  if (!Number.isFinite(value) || value < 0) return null
  const number = Number.isInteger(value) ? String(value) : String(Number(value.toFixed(4)))
  if (!plainDecimal(number)) return null
  return text.slice(0, word.begin) + number + text.slice(word.end)
}

/** Whether `after` is `before` with only the number of one F or S word changed. */
export function feedOrSpeedEdit(before: string, after: string): boolean {
  // eslint-disable-next-line no-control-regex
  if (/[\x00-\x08\x0a-\x1f\x7f]/.test(before + after)) return false
  return rateWords(before).some((word) => {
    const tail = before.length - word.end
    if (after.length < word.begin + tail || !after.startsWith(before.slice(0, word.begin)) ||
        !after.endsWith(before.slice(word.end))) return false
    const number = after.slice(word.begin, after.length - tail)
    return plainDecimal(number) && number !== before.slice(word.begin, word.end)
  })
}
