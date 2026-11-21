export type GcodeTone =
  | 'rapid' | 'cut' | 'modal' | 'x' | 'y' | 'z' | 'argument' | 'rate'
  | 'tool' | 'spindle' | 'stop' | 'comment' | 'message' | 'flow' | 'parameter' | 'expression'

export interface GcodeToken { text: string; tone: GcodeTone | null }

const number = /[ \t]*[+-]?[ \t]*(?:\d+(?:\.\d*)?|\.\d+)/y
const flow = /(?:endsub|endwhile|endrepeat|elseif|endif|continue|repeat|return|while|break|call|else|sub|if|do)\b/iy

function wordTone(letter: string, value: number): GcodeTone | null {
  switch (letter) {
    case 'G':
      if (value === 0) return 'rapid'
      if (value === 1 || value === 2 || value === 3 || Number.isInteger(value) && value >= 81 && value <= 89) return 'cut'
      return 'modal'
    case 'M':
      if (value === 6) return 'tool'
      if (value === 3 || value === 4 || value === 5 || value === 7 || value === 8 || value === 9) return 'spindle'
      if (value === 0 || value === 1 || value === 2 || value === 30 || value === 60) return 'stop'
      return 'modal'
    case 'X': return 'x'
    case 'Y': return 'y'
    case 'Z': return 'z'
    case 'I': case 'J': case 'K': case 'R': case 'Q': case 'P': return 'argument'
    case 'F': case 'S': return 'rate'
    case 'T': case 'H': return 'tool'
    case 'N': return 'modal'
    case 'O': return 'flow'
    default: return null
  }
}

function delimitedEnd(text: string, start: number, close: string): number {
  const end = text.indexOf(close, start + 1)
  return end === -1 ? text.length : end + 1
}

function expressionEnd(text: string, start: number): number {
  let depth = 1
  for (let i = start + 1; i < text.length; i += 1) {
    if (text[i] === '[') depth += 1
    if (text[i] === ']') {
      depth -= 1
      if (depth === 0) return i + 1
    }
  }
  return text.length
}

function isSpace(character: string | undefined): boolean {
  return character === ' ' || character === '\t'
}

function skipSpace(text: string, start: number): number {
  let end = start
  while (isSpace(text[end])) end += 1
  return end
}

/** Label syntax for display while preserving the source, including whitespace and unknown words. */
export function tokenizeGcodeLine(text: string): GcodeToken[] {
  const tokens: GcodeToken[] = []
  let start = 0
  let index = 0
  while (index < text.length) {
    const character = text.charAt(index)
    let end = index + 1
    let tone: GcodeTone | null = null
    if (character === ';') {
      end = text.length
      tone = 'comment'
    } else if (character === '(') {
      end = delimitedEnd(text, index, ')')
      tone = /^\(\s*msg\s*,/i.test(text.slice(index, end)) ? 'message' : 'comment'
    } else if (character === '[') {
      end = expressionEnd(text, index)
      tone = 'expression'
    } else if (character === '#') {
      while (text[end] === '#') end += 1
      const next = skipSpace(text, end)
      if (text[next] === '<') {
        end = delimitedEnd(text, next, '>')
      } else {
        number.lastIndex = end
        if (number.test(text)) end = number.lastIndex
      }
      tone = 'parameter'
    } else if (/[a-z]/i.test(character)) {
      flow.lastIndex = index
      const letter = character.toUpperCase()
      const valueAt = skipSpace(text, end)
      if (flow.test(text)) {
        end = flow.lastIndex
        tone = 'flow'
      } else if (letter === 'O' && text[valueAt] === '<') {
        end = delimitedEnd(text, valueAt, '>')
        tone = 'flow'
      } else {
        number.lastIndex = end
        if (number.test(text)) {
          end = number.lastIndex
          const value = letter === 'G' || letter === 'M'
            ? Number(text.slice(index + 1, end).replace(/[ \t]/g, '')) : Number.NaN
          tone = wordTone(letter, value)
        } else if (text[valueAt] === '[' || text[valueAt] === '#') {
          tone = wordTone(letter, Number.NaN)
        }
      }
    }
    if (tone !== null) {
      if (index > start) tokens.push({ text: text.slice(start, index), tone: null })
      tokens.push({ text: text.slice(index, end), tone })
      start = end
    }
    index = end
  }
  if (start < text.length) tokens.push({ text: text.slice(start), tone: null })
  return tokens
}
