// What every conversational operation shares: the program's unit, the tool, the speeds, the
// numbers as they are written, and the start and end of the program. Pure functions only: the
// dialog saves the text through the machine and opens it like any other program.

export type Unit = 'mm' | 'inch'

/** The tool from the tool table, its diameter in the program's unit. */
export interface ToolChoice {
  number: number
  diameter: number
  comment: string
}

/** Entries of every operation, in the program's unit (lengths, feeds per minute). */
export interface Common {
  unit: Unit
  tool: ToolChoice
  /** Spindle speed, rpm. */
  rpm: number
  /** Cutting feed. */
  feed: number
  /** Height above Z0 (top of the stock) for every move in the air. */
  clearance: number
}

/** The program text, or every reason there is none, in words for the operator. */
export type Generated = { text: string } | { errors: string[] }

const decimals = (unit: Unit): number => (unit === 'inch' ? 4 : 3)

/** A length or coordinate as written: fixed decimals for the unit, no trailing zeros, no -0. */
export function number(value: number, unit: Unit): string {
  const text = String(Number(value.toFixed(decimals(unit))))
  return text === '-0' ? '0' : text
}

/** A feed: one decimal in mm/min, two in in/min. */
export function feedNumber(value: number, unit: Unit): string {
  return String(Number(value.toFixed(unit === 'inch' ? 2 : 1)))
}

export function unitName(unit: Unit): string {
  return unit === 'inch' ? 'in' : 'mm'
}

/** Text that may go inside a ( ) comment: no parentheses, no control characters. */
export function commentText(text: string): string {
  // eslint-disable-next-line no-control-regex
  return text.replace(/[()\x00-\x1f\x7f]/g, ' ').replace(/\s+/g, ' ').trim()
}

/** Collects validation errors: `check(condition, message)` records the message when false. */
export class Checks {
  readonly errors: string[] = []
  check(condition: boolean, message: string): void {
    if (!condition) this.errors.push(message)
  }
  positive(value: number, label: string): void {
    this.check(Number.isFinite(value) && value > 0, `${label} must be more than zero.`)
  }
  count(value: number, label: string, max: number): void {
    this.check(Number.isInteger(value) && value >= 1 && value <= max, `${label} must be a whole number from 1 to ${String(max)}.`)
  }
}

/** Checks of the entries every operation has. */
export function checkCommon(c: Common, checks: Checks): void {
  checks.check(Number.isInteger(c.tool.number) && c.tool.number > 0, 'Choose a tool from the tool table.')
  checks.positive(c.tool.diameter, 'The tool diameter in the tool table')
  checks.positive(c.feed, 'Feed')
  checks.positive(c.rpm, 'Spindle speed')
  checks.positive(c.clearance, 'Clearance height')
}

/** How many equal steps of at most `step` make `total`: at least one. */
export function steps(total: number, step: number): number {
  return Math.max(1, Math.ceil(total / step - 1e-9))
}

/**
 * The lines of a program. Words are in the program's unit; `F` is written only when the feed
 * changes, so the program reads as a person would write it.
 */
export class Program {
  readonly lines: string[] = []
  private feed: number | null = null

  constructor(readonly c: Common) {}

  n(value: number): string {
    return number(value, this.c.unit)
  }

  add(...lines: string[]): void {
    this.lines.push(...lines)
  }

  /** `G1` or an arc with the feed word appended when it differs from the last one. */
  cut(words: string, feed: number): void {
    const f = feedNumber(feed, this.c.unit)
    const changed = this.feed === null || feedNumber(this.feed, this.c.unit) !== f
    this.feed = feed
    this.add(changed ? `${words} F${f}` : words)
  }

  rapidZ(z: number): void {
    this.add(`G0 Z${this.n(z)}`)
  }

  rapidXY(x: number, y: number): void {
    this.add(`G0 X${this.n(x)} Y${this.n(y)}`)
  }

  /** Header comments, then the modal codes, the tool change and the spindle start. */
  start(title: string, details: string[]): void {
    const { unit, tool } = this.c
    const comment = commentText(tool.comment)
    this.add(
      `(${title}, made by CoffeeCNC)`,
      `(Units: ${unitName(unit)}. Z0 is the top of the stock.)`,
      `(Tool: T${String(tool.number)}, diameter ${this.n(tool.diameter)}${comment ? `, ${comment}` : ''})`,
      ...details.map((line) => `(${commentText(line)})`),
      `(Feed ${feedNumber(this.c.feed, unit)}, spindle ${String(Math.round(this.c.rpm))} rpm, clearance Z${this.n(this.c.clearance)})`,
      '',
      `G17 ${unit === 'inch' ? 'G20' : 'G21'} G40 G80 G90 G94`,
      `T${String(tool.number)} M6`,
      `G43 H${String(tool.number)}`,
      `S${String(Math.round(this.c.rpm))} M3`,
    )
    this.rapidZ(this.c.clearance)
  }

  /** Clear of the work, spindle off, program end. */
  end(): string {
    if (this.lines.at(-1) !== `G0 Z${this.n(this.c.clearance)}`) this.rapidZ(this.c.clearance)
    this.add('M5', 'M2')
    return `${this.lines.join('\n')}\n`
  }
}
