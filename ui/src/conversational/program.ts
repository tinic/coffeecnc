// What every conversational operation shares: the program's unit, the tool, the speeds, the
// numbers as they are written, and the start and end of the program. Pure functions only: the
// dialog saves the text through the machine and opens it like any other program.

import { t, type MessageSchema } from '../i18n'

export type Unit = 'mm' | 'inch'

type Operations = MessageSchema['operations']
/** A message key of one group of the operations catalog, translated where it is shown. */
export type OperationsKey<G extends keyof Operations> = `operations.${G}.${keyof Operations[G] & string}`
/** The name of an entry, as a validation message names it. */
export type LabelKey = OperationsKey<'label'>

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

const programEnd = 'M2'

export interface ChainOperation {
  tool: number
  text: string
}

/** Joins complete generator outputs. The tool-change height is in program units, machine Z. */
export function chainOperations(operations: readonly ChainOperation[], unit: Unit, toolChangeZ: number | null = null): Generated {
  const end = `${programEnd}\n`
  const toolBlock = (tool: number): string => `\nT${String(tool)} M6\n`
  if (operations.length === 0 || operations.some(({ tool, text }) => !text.endsWith(`\n${end}`) || !text.includes(toolBlock(tool))))
    return { errors: [t('operations.check.invalidJob')] }
  const sections: string[] = []
  let previousTool: number | undefined
  for (const { tool, text } of operations) {
    let section = text.slice(0, -end.length)
    if (previousTool === tool) {
      section = section.replace(toolBlock(tool), '\n')
    } else if (previousTool !== undefined) {
      if (toolChangeZ === null || !Number.isFinite(toolChangeZ))
        return { errors: [t('operations.check.jobToolChangeHeight')] }
      section = `G90 G53 G0 Z${number(toolChangeZ, unit)}\n\n${section}`
    }
    sections.push(section)
    previousTool = tool
  }
  return { text: sections.join('\n') + end }
}

const decimals = (unit: Unit): number => (unit === 'inch' ? 4 : 3)

/** A length or coordinate as written: fixed decimals for the unit, no trailing zeros, no -0. */
export function number(value: number, unit: Unit): string {
  const text = String(Number(value.toFixed(decimals(unit))))
  return text === '-0' ? '0' : text
}

/**
 * The value as written. An arc radius is checked as written: one that is more than zero but
 * written as 0 makes `I0 J0`, which the interpreter refuses.
 */
export function written(value: number, unit: Unit): number {
  return Number(number(value, unit))
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

/**
 * Collects validation errors in the panel's language: `check(condition, message)` records the
 * message when false. Generators run again when the language changes, so the text follows it.
 */
export class Checks {
  readonly errors: string[] = []
  check(condition: boolean, message: string): void {
    if (!condition) this.errors.push(message)
  }
  positive(value: number, label: LabelKey): void {
    if (!(Number.isFinite(value) && value > 0)) this.errors.push(t('operations.check.positive', { label: t(label) }))
  }
  count(value: number, label: LabelKey, max: number): void {
    if (!(Number.isInteger(value) && value >= 1 && value <= max))
      this.errors.push(t('operations.check.count', { label: t(label), max: String(max) }))
  }
}

/** Checks of the entries every operation has. */
export function checkCommon(c: Common, checks: Checks): void {
  if (!(Number.isInteger(c.tool.number) && c.tool.number > 0)) checks.errors.push(t('operations.check.chooseTool'))
  checks.positive(c.tool.diameter, 'operations.label.toolDiameter')
  checks.positive(c.feed, 'operations.label.feed')
  checks.positive(c.rpm, 'operations.label.spindleSpeed')
  checks.positive(c.clearance, 'operations.label.clearance')
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
      `(${title}, made by CoffeeCNC)`, // i18n-ignore
      `(Units: ${unitName(unit)}. Z0 is the top of the stock.)`, // i18n-ignore
      `(Tool: T${String(tool.number)}, diameter ${this.n(tool.diameter)}${comment ? `, ${comment}` : ''})`, // i18n-ignore
      ...details.map((line) => `(${commentText(line)})`),
      `(Feed ${feedNumber(this.c.feed, unit)}, spindle ${String(Math.round(this.c.rpm))} rpm, clearance Z${this.n(this.c.clearance)})`, // i18n-ignore
      '',
      `G17 ${unit === 'inch' ? 'G20' : 'G21'} G40 G80 G90 G94`,
      `T${String(tool.number)} M6`,
      `G43 H${String(tool.number)}`,
      `S${String(Math.round(this.c.rpm))} M3`,
    )
    this.rapidZ(this.c.clearance)
  }

  /** Clear of the work, then any final modal cleanup, spindle off and program end. */
  end(stopCoolant = false, afterClearance: readonly string[] = []): string {
    if (this.lines.at(-1) !== `G0 Z${this.n(this.c.clearance)}`) this.rapidZ(this.c.clearance)
    if (stopCoolant) this.add('M9')
    this.add(...afterClearance, 'M5', programEnd)
    return `${this.lines.join('\n')}\n`
  }
}
